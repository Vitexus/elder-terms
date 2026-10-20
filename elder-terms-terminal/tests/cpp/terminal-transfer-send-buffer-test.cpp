#include "../../src/terminal-transfer-send-buffer.h"

#include <algorithm>
#include <array>
#include <exception>
#include <stdexcept>
#include <vector>

namespace elder_terms {

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static cardio::promise<void> test_tail_and_full_batch() {
  TerminalTransferSendBuffer buffer;
  std::vector<std::uint8_t> actual;
  std::vector<std::size_t> write_sizes;
  const auto now = [] { return std::uint64_t{0}; };
  const auto send = [&](std::span<const std::uint8_t> bytes,
      std::uint32_t, std::size_t &written, cardio::cancellation)
      -> cardio::promise<void> {
    write_sizes.push_back(bytes.size());
    actual.insert(actual.end(), bytes.begin(), bytes.end());
    written = bytes.size();
    co_return;
  };
  const std::array<std::uint8_t, 3> first{1, 2, 3};
  std::size_t written = 0;
  co_await buffer.send_async(first, 5000, written, send, now, {});
  expect(written == first.size() && actual.empty(),
         "short writes should be accepted without a transport call");
  co_await buffer.flush_async(send, now, {});
  expect(actual == std::vector<std::uint8_t>(first.begin(), first.end()) &&
             write_sizes == std::vector<std::size_t>{first.size()},
         "explicit flush should write a short tail once");
  co_await buffer.flush_async(send, now, {});
  expect(write_sizes.size() == 1, "empty flush should not repeat output");

  std::vector<std::uint8_t> full(TerminalTransferSendBuffer::capacity, 0x51);
  co_await buffer.send_async(full, 5000, written, send, now, {});
  expect(written == full.size() && write_sizes.size() == 2 &&
             write_sizes.back() == TerminalTransferSendBuffer::capacity &&
             std::equal(full.begin(), full.end(), actual.end() - full.size()),
         "a full batch should flush once and preserve byte order");

  std::vector<std::uint8_t> oversized(TerminalTransferSendBuffer::capacity + 7, 0x62);
  co_await buffer.send_async(oversized, 5000, written, send, now, {});
  expect(written == TerminalTransferSendBuffer::capacity && write_sizes.size() == 3,
         "input larger than capacity should return a bounded accepted prefix");
  co_await buffer.send_async(std::span(oversized).subspan(written), 5000,
                             written, send, now, {});
  expect(written == 7 && write_sizes.size() == 3,
         "remaining oversized input should stay buffered until flush");
  co_await buffer.flush_async(send, now, {});
  expect(write_sizes.size() == 4 && write_sizes.back() == 7 &&
             std::equal(oversized.begin(), oversized.end(), actual.end() - oversized.size()),
         "oversized input should reach the transport exactly once in order");
}

static cardio::promise<void> test_partial_writes_and_short_acceptance() {
  TerminalTransferSendBuffer buffer;
  std::uint64_t clock = 100;
  const auto now = [&] { return clock; };
  std::vector<std::uint32_t> timeouts;
  std::vector<std::uint8_t> actual;
  const auto send = [&](std::span<const std::uint8_t> bytes,
      std::uint32_t timeout, std::size_t &written, cardio::cancellation)
      -> cardio::promise<void> {
    timeouts.push_back(timeout);
    written = std::min<std::size_t>(2, bytes.size());
    actual.insert(actual.end(), bytes.begin(), bytes.begin() + written);
    clock += 2;
    co_return;
  };
  const std::array<std::uint8_t, 6> payload{1, 2, 3, 4, 5, 6};
  std::size_t written = 0;
  co_await buffer.send_async(payload, 30, written, send, now, {});
  co_await buffer.flush_async(send, now, {});
  expect(written == payload.size() && actual == std::vector<std::uint8_t>(payload.begin(), payload.end()) &&
             timeouts == std::vector<std::uint32_t>{30, 28, 26},
         "partial writes should finish the batch under one decreasing deadline");

  TerminalTransferSendBuffer short_buffer;
  std::vector<std::uint8_t> first(TerminalTransferSendBuffer::capacity - 1, 0x22);
  const std::array<std::uint8_t, 2> second{0x33, 0x44};
  std::size_t full_writes = 0;
  const auto write_full = [&](std::span<const std::uint8_t> bytes,
      std::uint32_t timeout, std::size_t &count, cardio::cancellation)
      -> cardio::promise<void> {
    expect(timeout == 10, "full queue should flush under the shorter accepted timeout");
    ++full_writes;
    count = bytes.size();
    co_return;
  };
  co_await short_buffer.send_async(first, 30, written, write_full, now, {});
  co_await short_buffer.send_async(second, 10, written, write_full, now, {});
  expect(written == 1 && full_writes == 1,
         "a full queue should return a short accepted count after flushing");
}

static cardio::promise<void> test_invalid_write_and_abort() {
  const auto now = [] { return std::uint64_t{0}; };
  const std::array<std::uint8_t, 3> payload{1, 2, 3};
  const std::array<std::uint8_t, 2> can{0x18, 0x18};
  for (const bool oversized : {false, true}) {
    TerminalTransferSendBuffer buffer;
    std::size_t written = 0;
    std::size_t calls = 0;
    const auto broken = [&](std::span<const std::uint8_t> bytes,
        std::uint32_t, std::size_t &count, cardio::cancellation)
        -> cardio::promise<void> {
      ++calls;
      count = oversized ? bytes.size() + 1 : 0;
      co_return;
    };
    co_await buffer.send_async(payload, 5000, written, broken, now, {});
    bool failed = false;
    try { co_await buffer.flush_async(broken, now, {}); }
    catch (const xyzm_async_io_error &) { failed = true; }
    expect(failed && calls == 1, "invalid transport write should fail one batch");
    std::vector<std::uint8_t> actual;
    const auto direct = [&](std::span<const std::uint8_t> bytes,
        std::uint32_t, std::size_t &count, cardio::cancellation)
        -> cardio::promise<void> {
      actual.insert(actual.end(), bytes.begin(), bytes.end());
      count = bytes.size();
      co_return;
    };
    co_await buffer.send_async(can, 5000, written, direct, now, {});
    co_await buffer.flush_async(direct, now, {});
    expect(written == can.size() && actual == std::vector<std::uint8_t>(can.begin(), can.end()),
           "a failed batch must be discarded and later CAN sent directly");
  }
}

static cardio::promise<void> test_timeout_and_cancellation() {
  TerminalTransferSendBuffer buffer;
  std::uint64_t clock = 0;
  const auto now = [&] { return clock; };
  const std::array<std::uint8_t, 4> payload{1, 2, 3, 4};
  std::size_t written = 0;
  co_await buffer.send_async(payload, 20, written,
      [](std::span<const std::uint8_t>, std::uint32_t,
         std::size_t &, cardio::cancellation) -> cardio::promise<void> { co_return; }, now, {});
  const std::array<std::uint8_t, 2> extra{5, 6};
  co_await buffer.send_async(extra, 5, written,
      [](std::span<const std::uint8_t>, std::uint32_t,
         std::size_t &, cardio::cancellation) -> cardio::promise<void> { co_return; }, now, {});
  std::size_t calls = 0;
  const auto slow = [&](std::span<const std::uint8_t> bytes,
      std::uint32_t timeout, std::size_t &count, cardio::cancellation)
      -> cardio::promise<void> {
    expect(timeout == 5, "flush should use the shortest accepted timeout");
    ++calls;
    count = 1;
    clock += 5;
    (void)bytes;
    co_return;
  };
  bool timed_out = false;
  try { co_await buffer.flush_async(slow, now, {}); }
  catch (const xyzm_async_timeout_error &) { timed_out = true; }
  expect(timed_out && calls == 1, "one batch deadline should cover all partial writes");

  TerminalTransferSendBuffer cancelled_buffer;
  co_await cancelled_buffer.send_async(payload, 5000, written,
      [](std::span<const std::uint8_t>, std::uint32_t,
         std::size_t &, cardio::cancellation) -> cardio::promise<void> { co_return; }, now, {});
  cardio::cancellation_source stop;
  stop.cancel();
  bool cancelled = false;
  try { co_await cancelled_buffer.flush_async(slow, now, stop.get_cancellation()); }
  catch (const cardio::canceled_exception &) { cancelled = true; }
  expect(cancelled && calls == 1, "cancelled flush must not call the transport");

  std::vector<std::uint8_t> actual;
  const auto direct = [&](std::span<const std::uint8_t> bytes,
      std::uint32_t, std::size_t &count, cardio::cancellation)
      -> cardio::promise<void> {
    actual.insert(actual.end(), bytes.begin(), bytes.end());
    count = bytes.size();
    co_return;
  };
  const std::array<std::uint8_t, 2> can{0x18, 0x18};
  co_await cancelled_buffer.send_async(can, 5000, written, direct, now, {});
  expect(actual == std::vector<std::uint8_t>(can.begin(), can.end()),
         "cancelled output must not replay pending bytes before CAN");

  TerminalTransferSendBuffer aborted;
  co_await aborted.send_async(payload, 5000, written, direct, now, {});
  aborted.abort();
  actual.clear();
  co_await aborted.send_async(can, 5000, written, direct, now, {});
  expect(actual == std::vector<std::uint8_t>(can.begin(), can.end()),
         "explicit abort should send CAN without a pending payload");
}

static cardio::promise<void> run_tests(cardio::dispatcher_group_glib &group,
                                        std::exception_ptr &failure) {
  try {
    co_await test_tail_and_full_batch();
    co_await test_partial_writes_and_short_acceptance();
    co_await test_invalid_write_and_abort();
    co_await test_timeout_and_cancellation();
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
  elder_terms::expect(task.try_result(), "send buffer tests should complete");
  if (failure) std::rethrow_exception(failure);
  return 0;
}
