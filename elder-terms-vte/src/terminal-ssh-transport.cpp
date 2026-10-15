#include "terminal-transports.h"
#include "terminal-sessions/ssh-session/ssh-channel-connection.h"

namespace elder_terms {

static cardio::promise<TerminalTransport> open_ssh(SshConnectionSettings settings,
    TerminalSessionCallbacks callbacks, AuthenticatedSshTransportOptions options,
    std::size_t rows, std::size_t columns, cardio::cancellation cancellation) {
  auto connecting = SshChannelConnection::connect_async(settings, static_cast<glong>(columns),
      static_cast<glong>(rows), callbacks, std::move(options), cancellation);
  std::shared_ptr<SshChannelConnection> channel(std::move(co_await connecting));
  co_return TerminalTransport{
      [channel](std::span<unsigned char> bytes, cardio::cancellation stop) { return channel->read_async(bytes, stop); },
      [channel](std::span<const unsigned char> bytes, cardio::cancellation stop) { return channel->write_all_async(bytes, stop); },
      [channel](std::size_t height, std::size_t width, cardio::cancellation stop) {
        return channel->resize_async(static_cast<glong>(width), static_cast<glong>(height), stop);
      },
      [channel]() -> cardio::promise<void> { channel->close(); co_return; }, {},
      [channel](cardio::cancellation stop) { return channel->send_break_async(500, stop); }};
}

TerminalTransportRequest make_ssh_transport_request(SshConnectionSettings settings,
    std::string encoding, TerminalSessionCallbacks callbacks, AuthenticatedSshTransportOptions options) {
  TerminalTransportRequest request;
  request.encoding = std::move(encoding);
  request.open = [settings = std::move(settings), callbacks = std::move(callbacks), options = std::move(options)](
      std::size_t rows, std::size_t columns, cardio::cancellation cancellation) {
    return open_ssh(settings, callbacks, options, rows, columns, cancellation);
  };
  return request;
}

}
