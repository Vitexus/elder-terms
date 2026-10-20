#pragma once

#include <cardio.h>
#include <sys/socket.h>
#include <cerrno>
#include <cstddef>
#include <span>
#include <system_error>

namespace elder_terms {

/**
 * Receives available socket bytes without blocking the dispatcher.
 * @param fd Socket retained by the caller until completion.
 * @param bytes Destination retained by the caller until completion.
 * @param cancellation Cancellation signal for the read.
 * @return Bytes received, or zero when the peer closes its output.
 */
inline cardio::promise<std::size_t> read_terminal_socket_async(
    int fd, std::span<unsigned char> bytes, cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  if (bytes.empty()) co_return 0;
  for (;;) {
    cancellation.throw_if_cancellation_requested();
    const auto count = ::recv(fd, bytes.data(), bytes.size(), MSG_DONTWAIT);
    if (count >= 0) co_return static_cast<std::size_t>(count);
    const auto error = errno;
    if (error == EINTR) continue;
    if (error != EAGAIN && error != EWOULDBLOCK)
      throw std::system_error(error, std::generic_category(), "read TELNET");
    co_await cardio::from_fd(fd, cardio::fd_event::read, cancellation);
  }
}

} // namespace elder_terms
