#include "terminal-transports.h"
#include "terminal-sessions/serial-session/serial-termios.h"
#include "terminal-sessions/serial-session/serial-line-monitor.h"

#include <elder-terms/serial-device.h>
#include <elder-terms/serial-device-event-monitor.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <memory>
#include <iostream>
#include <poll.h>
#include <system_error>
#include <utility>

namespace elder_terms {

struct SerialChannel {
  cardio::io_uring io{16};
  int descriptor = -1;
  TerminalSessionCallbacks callbacks;

  ~SerialChannel() { if (descriptor >= 0) ::close(descriptor); }
};

static cardio::promise<std::size_t> read_serial(SerialChannel *channel,
    std::span<unsigned char> bytes, cardio::cancellation cancellation) {
  for (;;) {
    co_await cardio::from_fd(channel->descriptor, cardio::fd_event::read, cancellation);
    const auto count = ::read(channel->descriptor, bytes.data(), bytes.size());
    if (count > 0) {
      if (channel->callbacks.activity) channel->callbacks.activity(ActivityIndicatorId::rd);
      co_return static_cast<std::size_t>(count);
    }
    if (count == 0 || errno == EIO) co_return 0;
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
      throw std::system_error(errno, std::generic_category(), "read serial port");
  }
}

static cardio::promise<void> write_serial(SerialChannel *channel,
    std::span<const unsigned char> bytes, cardio::cancellation cancellation) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    co_await cardio::from_fd(channel->descriptor, cardio::fd_event::write, cancellation);
    const auto count = ::write(channel->descriptor, bytes.data() + offset, bytes.size() - offset);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      if (channel->callbacks.activity) channel->callbacks.activity(ActivityIndicatorId::sd);
    } else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
      throw std::system_error(count == 0 ? EIO : errno, std::generic_category(), "write serial port");
  }
}

static cardio::promise<void> close_serial(SerialChannel *channel) {
  if (channel->callbacks.indicator_state)
    for (const auto state : serial_line_indicator_states({}))
      channel->callbacks.indicator_state(state.indicator, state.active);
  if (channel->descriptor >= 0)
    co_await cardio::io_urings::close(channel->io, std::exchange(channel->descriptor, -1));
}

static cardio::promise<void> watch_serial_carrier(SerialChannel *channel, SerialCarrierDetect selected,
    std::function<void(bool)> notify, cardio::cancellation cancellation) {
  if (selected == SerialCarrierDetect::ignore) co_return;
  SerialCarrierTracker tracker(selected);
  bool connected = true;
  try {
    for (;;) {
      cancellation.throw_if_cancellation_requested();
      const auto signals = read_serial_line_signals(channel->descriptor);
      if (channel->callbacks.indicator_state)
        for (const auto state : serial_line_indicator_states(signals))
          channel->callbacks.indicator_state(state.indicator, state.active);
      const bool high = selected == SerialCarrierDetect::cts ? signals.cts :
          selected == SerialCarrierDetect::dsr ? signals.dsr : signals.cd;
      if (high) connected = true;
      if (tracker.update(signals) == SerialCarrierEvent::disconnected) connected = false;
      notify(connected);
      const auto events = co_await channel->io.submit<short>(
          [descriptor = channel->descriptor](::io_uring_sqe *sqe) {
            ::io_uring_prep_poll_add(sqe, descriptor, POLLPRI | POLLERR | POLLHUP | POLLNVAL);
          },
          [](cardio::io_uring_completion completion) {
            if (completion.result < 0)
              throw std::system_error(-completion.result, std::generic_category(), "serial modem-line poll");
            return static_cast<short>(completion.result);
          }, cancellation);
      if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        throw std::system_error(ENODEV, std::generic_category(), "serial modem-line hangup");
    }
  } catch (const std::system_error &error) {
    const auto code = error.code().value();
    if (code == EIO || code == ENODEV || code == EBADF) throw;
    // PTYs and some USB adapters do not expose modem signals. Preserve their
    // byte stream and report the unsupported monitor once per opened device.
    std::cerr << "Warning: serial carrier detection unavailable: " << error.what() << '\n';
  }
}

static cardio::promise<void> break_serial(SerialChannel *channel, cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  set_serial_break_state(channel->descriptor, true);
  std::exception_ptr failure;
  try { co_await cardio::promises::delay(500, cancellation); }
  catch (...) { failure = std::current_exception(); }
  // The connection joins this operation before closing the descriptor, even
  // when disconnect interrupts the asserted interval.
  set_serial_break_state(channel->descriptor, false);
  if (failure) std::rethrow_exception(failure);
}

static cardio::promise<TerminalTransport> open_serial(SerialConnectionSettings settings,
    TerminalSessionCallbacks callbacks, cardio::cancellation cancellation) {
  auto resolving = cardio::promises::start_new([settings] {
    return resolve_serial_device(settings.device_match_mode, settings.device, settings.device_usb_serial);
  });
  const auto resolved = co_await resolving;
  cancellation.throw_if_cancellation_requested();
  if (!resolved.resolved)
    throw std::system_error(ENOENT, std::generic_category(), resolved.warnings.empty() ?
        "serial device is unavailable" : resolved.warnings.front());
  auto channel = std::make_shared<SerialChannel>();
  channel->callbacks = std::move(callbacks);
  channel->descriptor = co_await cardio::io_urings::open(channel->io, resolved.path.string(),
      O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC, 0, cancellation);
  cancellation.throw_if_cancellation_requested();
  configure_serial_port(channel->descriptor, settings);
  co_return TerminalTransport{
      [channel](std::span<unsigned char> bytes, cardio::cancellation stop) { return read_serial(channel.get(), bytes, stop); },
      [channel](std::span<const unsigned char> bytes, cardio::cancellation stop) { return write_serial(channel.get(), bytes, stop); },
      [](std::size_t, std::size_t, cardio::cancellation stop) -> cardio::promise<void> { stop.throw_if_cancellation_requested(); co_return; },
      [channel] { return close_serial(channel.get()); }, {},
      [channel](cardio::cancellation stop) { return break_serial(channel.get(), stop); }, {},
      [channel, selected = settings.carrier_detect](std::function<void(bool)> notify, cardio::cancellation stop) {
        return watch_serial_carrier(channel.get(), selected, std::move(notify), stop);
      }};
}

static cardio::promise<void> watch_serial_device(std::string device,
    std::function<void()> notify, cardio::cancellation cancellation) {
  cardio::primitives::conditional ready;
  bool changed = false;
  SerialDeviceEventMonitor monitor(std::move(device), [&] {
    changed = true;
    ready.trigger();
  });
  monitor.start();
  if (!monitor.has_event_sources()) co_return;
  try {
    for (;;) {
      while (!changed) co_await ready.wait(cancellation);
      cancellation.throw_if_cancellation_requested();
      changed = false;
      notify();
    }
  } catch (const cardio::canceled_exception &) {
  }
  monitor.stop();
}

TerminalTransportRequest make_serial_transport_request(SerialConnectionSettings settings,
    std::string encoding, TerminalSessionCallbacks callbacks) {
  TerminalTransportRequest request;
  request.encoding = std::move(encoding);
  request.watch_availability = [device = settings.device](std::function<void()> notify, cardio::cancellation cancellation) {
    return watch_serial_device(device, std::move(notify), cancellation);
  };
  request.open = [settings = std::move(settings), callbacks = std::move(callbacks)](
      std::size_t, std::size_t, cardio::cancellation cancellation) {
    return open_serial(settings, callbacks, cancellation);
  };
  return request;
}

}
