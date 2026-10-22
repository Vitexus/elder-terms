// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/terminal-transfer-sink-buffer.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <vector>

namespace elder_terms {

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static cardio::promise<void> test_buffered_payload_and_tail() {
  std::vector<std::uint8_t> payload(150 * 1024 + 17);
  for (std::size_t index = 0; index < payload.size(); ++index)
    payload[index] = static_cast<std::uint8_t>(index * 37);
  std::vector<std::uint8_t> actual;
  std::vector<std::size_t> writes;
  auto write = [&](std::span<const std::uint8_t> bytes,
                   cardio::cancellation) -> cardio::promise<void> {
    writes.push_back(bytes.size());
    actual.insert(actual.end(), bytes.begin(), bytes.end());
    co_return;
  };
  TerminalTransferSinkBuffer buffer;
  for (std::size_t offset = 0; offset < payload.size();) {
    const auto count = std::min<std::size_t>(1024, payload.size() - offset);
    co_await buffer.append_async(std::span(payload).subspan(offset, count), write, {});
    offset += count;
  }
  const auto calls_before_end = writes.size();
  co_await buffer.flush_async(write, {});
  expect(actual == payload, "sink should preserve all accepted bytes including its tail");
  expect(calls_before_end == 2 && writes.size() == 3,
         "150 KiB in small packets should need only three file writes");
  expect(writes[0] == TerminalTransferSinkBuffer::capacity &&
             writes[1] == TerminalTransferSinkBuffer::capacity &&
             writes[2] == 22 * 1024 + 17,
         "writes should remain bounded and preserve the final short tail");
  co_await buffer.flush_async(write, {});
  expect(writes.size() == 3, "empty flush should not rewrite bytes");
}

static cardio::promise<void> test_cancelled_input_and_failed_flush() {
  const std::vector<std::uint8_t> accepted{1, 2, 3, 4};
  const std::vector<std::uint8_t> cancelled_input{5, 6};
  std::vector<std::uint8_t> actual;
  auto write = [&](std::span<const std::uint8_t> bytes,
                   cardio::cancellation) -> cardio::promise<void> {
    actual.insert(actual.end(), bytes.begin(), bytes.end());
    co_return;
  };
  TerminalTransferSinkBuffer buffer;
  co_await buffer.append_async(accepted, write, {});
  cardio::cancellation_source stop;
  stop.cancel();
  bool cancelled = false;
  try { co_await buffer.append_async(cancelled_input, write, stop.get_cancellation()); }
  catch (const cardio::canceled_exception &) { cancelled = true; }
  expect(cancelled, "cancelled input must not be accepted");
  co_await buffer.flush_async(write, {});
  expect(actual == accepted, "finalization should preserve only previously accepted bytes");

  actual.clear();
  buffer.reset();
  co_await buffer.append_async(accepted, write, {});
  std::size_t failed_writes = 0;
  auto fail = [&](std::span<const std::uint8_t> bytes,
                  cardio::cancellation) -> cardio::promise<void> {
    ++failed_writes;
    actual.push_back(bytes.front());
    throw std::runtime_error("file write failed after its first byte");
    co_return;
  };
  bool failed = false;
  try { co_await buffer.flush_async(fail, {}); }
  catch (const std::runtime_error &) { failed = true; }
  expect(failed, "failed finalization must report the error");
  co_await buffer.flush_async(fail, {});
  expect(failed_writes == 1 && actual == std::vector<std::uint8_t>{1},
         "cleanup after a failed write must not repeat accepted file bytes");

  buffer.reset();
  actual.clear();
  co_await buffer.append_async(cancelled_input, write, {});
  co_await buffer.flush_async(write, {});
  expect(actual == cancelled_input, "a new file should not retain earlier payload");
}

static cardio::promise<void> run_tests(cardio::dispatcher_group_glib &group,
                                        std::exception_ptr &failure) {
  try {
    co_await test_buffered_payload_and_tail();
    co_await test_cancelled_input_and_failed_flush();
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
  elder_terms::expect(task.try_result(), "sink buffer tests should complete");
  if (failure) std::rethrow_exception(failure);
  return 0;
}
