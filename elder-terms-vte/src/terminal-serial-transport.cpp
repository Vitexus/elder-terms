#include "terminal-transports.h"
#include "terminal-sessions/serial-session/serial-termios.h"

#include <elder-terms/serial-device.h>
#include <elder-terms/serial-device-event-monitor.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <memory>
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
  if (channel->descriptor >= 0)
    co_await cardio::io_urings::close(channel->io, std::exchange(channel->descriptor, -1));
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
      [channel](cardio::cancellation stop) { return break_serial(channel.get(), stop); }};
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
