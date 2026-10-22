// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/terminal-socket-read.h"

#include <unistd.h>
#include <algorithm>
#include <array>
#include <exception>
#include <stdexcept>

namespace elder_terms {

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

struct SocketPair {
  int fds[2]{-1, -1};
  SocketPair() {
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) != 0)
      throw std::system_error(errno, std::generic_category(), "socketpair");
  }
  ~SocketPair() {
    for (const auto fd : fds) if (fd >= 0) ::close(fd);
  }
};

static cardio::promise<void> test_ready_read() {
  SocketPair sockets;
  const std::array<unsigned char, 4> payload{0, 1, 254, 255};
  expect(::send(sockets.fds[0], payload.data(), payload.size(), MSG_NOSIGNAL) == 4,
         "prepare binary input");
  std::array<unsigned char, 3> received{};
  auto reading = read_terminal_socket_async(sockets.fds[1], received, {});
  const auto immediate_read = reading.is_ready();
  expect(co_await reading == received.size(), "read should fill the available destination");
  expect(std::equal(received.begin(), received.end(), payload.begin()),
         "read should preserve the binary prefix");
  expect(co_await read_terminal_socket_async(sockets.fds[1], received, {}) == 1 &&
             received[0] == 255, "short read should return the remaining byte");
  expect(immediate_read, "ready socket should return bytes without a dispatcher wait");
}

static cardio::promise<void> test_wait_cancellation_and_close() {
  SocketPair sockets;
  std::array<unsigned char, 1> byte{42};
  auto reading = read_terminal_socket_async(sockets.fds[1], byte, {});
  expect(!reading.is_ready(), "empty socket should wait for input");
  expect(::send(sockets.fds[0], byte.data(), byte.size(), MSG_NOSIGNAL) == 1,
         "writer should provide awaited input");
  expect(co_await reading == 1 && byte[0] == 42, "pending read should resume with data");

  cardio::cancellation_source stop;
  auto waiting = read_terminal_socket_async(sockets.fds[1], byte, stop.get_cancellation());
  stop.cancel();
  bool cancelled = false;
  try { (void)co_await waiting; }
  catch (const cardio::canceled_exception &) { cancelled = true; }
  expect(cancelled, "cancellation should stop a pending read");
  expect(::send(sockets.fds[0], byte.data(), byte.size(), MSG_NOSIGNAL) == 1,
         "prepare input for a pre-cancelled read");
  cancelled = false;
  try { (void)co_await read_terminal_socket_async(sockets.fds[1], byte, stop.get_cancellation()); }
  catch (const cardio::canceled_exception &) { cancelled = true; }
  expect(cancelled, "pre-cancelled read must fail");
  expect(co_await read_terminal_socket_async(sockets.fds[1], byte, {}) == 1 && byte[0] == 42,
         "pre-cancelled read must not consume bytes");

  auto ending = read_terminal_socket_async(sockets.fds[1], byte, {});
  expect(!ending.is_ready(), "read should wait before peer closes");
  ::close(sockets.fds[0]);
  sockets.fds[0] = -1;
  expect(co_await ending == 0, "peer closure should finish a pending read with EOF");
  expect(co_await read_terminal_socket_async(sockets.fds[1], byte, {}) == 0,
         "EOF should remain visible");
}

static cardio::promise<void> run_tests(cardio::dispatcher_group_glib &group,
                                        std::exception_ptr &failure) {
  try {
    co_await test_ready_read();
    co_await test_wait_cancellation_and_close();
  } catch (...) { failure = std::current_exception(); }
  group.shutdown();
}

} // namespace elder_terms

int main() {
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib dispatcher(group);
  std::exception_ptr failure;
  auto task = elder_terms::run_tests(group, failure);
  dispatcher.park();
  elder_terms::expect(task.try_result(), "socket read tests should complete");
  if (failure) std::rethrow_exception(failure);
  return 0;
}
