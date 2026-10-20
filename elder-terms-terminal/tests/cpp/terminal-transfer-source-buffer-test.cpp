#include "../../src/terminal-transfer-source-buffer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <vector>

namespace elder_terms {

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static cardio::promise<void> test_buffered_reads() {
  std::vector<std::uint8_t> payload(150 * 1024 + 17);
  for (std::size_t index = 0; index < payload.size(); ++index)
    payload[index] = static_cast<std::uint8_t>(index * 37);
  std::size_t source_offset = 0;
  std::size_t fill_count = 0;
  std::size_t largest_fill_request = 0;
  auto fill = [&](std::span<std::uint8_t> target,
                  cardio::cancellation) -> cardio::promise<std::size_t> {
    ++fill_count;
    largest_fill_request = std::max(largest_fill_request, target.size());
    const auto count = std::min(target.size(), payload.size() - source_offset);
    std::copy_n(payload.begin() + source_offset, count, target.begin());
    source_offset += count;
    co_return count;
  };
  TerminalTransferSourceBuffer buffer;
  std::array<std::uint8_t, 1024> destination{};
  std::vector<std::uint8_t> actual;
  std::size_t reported_bytes = 0;
  for (;;) {
    const auto count = co_await buffer.read_async(destination, fill, {});
    if (count == 0) break;
    actual.insert(actual.end(), destination.begin(), destination.begin() + count);
    reported_bytes += count;
  }
  expect(actual == payload, "buffered source should return every byte in order");
  expect(reported_bytes == payload.size(),
         "source progress should count consumed bytes once");
  expect(largest_fill_request == TerminalTransferSourceBuffer::capacity,
         "source should refill a 64 KiB buffer");
  expect(fill_count == 4,
         "150 KiB source should need three fills and one EOF read");
  expect(co_await buffer.read_async(destination, fill, {}) == 0,
         "EOF should remain visible");
  expect(fill_count == 4, "EOF should not request another fill");
}

static cardio::promise<void> test_short_read_and_empty_file() {
  const std::array<std::uint8_t, 7> payload{1, 2, 3, 4, 5, 6, 7};
  std::size_t source_offset = 0;
  std::size_t fill_count = 0;
  auto short_fill = [&](std::span<std::uint8_t> target,
                        cardio::cancellation) -> cardio::promise<std::size_t> {
    ++fill_count;
    const auto count = std::min({target.size(), payload.size() - source_offset,
                                 std::size_t{3}});
    std::copy_n(payload.begin() + source_offset, count, target.begin());
    source_offset += count;
    co_return count;
  };
  TerminalTransferSourceBuffer buffer;
  std::array<std::uint8_t, 5> destination{};
  std::vector<std::uint8_t> actual;
  for (;;) {
    const auto count = co_await buffer.read_async(destination, short_fill, {});
    if (count == 0) break;
    actual.insert(actual.end(), destination.begin(), destination.begin() + count);
  }
  expect(std::equal(actual.begin(), actual.end(), payload.begin(), payload.end()),
         "short fills should preserve the complete stream");
  expect(fill_count == 4, "short fills should reach EOF once");

  buffer.reset();
  std::size_t empty_fill_count = 0;
  auto empty_fill = [&](std::span<std::uint8_t>,
                        cardio::cancellation) -> cardio::promise<std::size_t> {
    ++empty_fill_count;
    co_return 0;
  };
  expect(co_await buffer.read_async(destination, empty_fill, {}) == 0,
         "empty source should end immediately");
  expect(co_await buffer.read_async(destination, empty_fill, {}) == 0,
         "empty source should stay at EOF");
  expect(empty_fill_count == 1, "empty source should be probed only once");
}

static cardio::promise<void> test_seek_file_switch_and_cancellation() {
  std::vector<std::uint8_t> first(128 * 1024);
  for (std::size_t index = 0; index < first.size(); ++index)
    first[index] = static_cast<std::uint8_t>(index);
  std::size_t source_offset = 0;
  std::size_t first_fill_count = 0;
  auto first_fill = [&](std::span<std::uint8_t> target,
                        cardio::cancellation) -> cardio::promise<std::size_t> {
    ++first_fill_count;
    const auto count = std::min(target.size(), first.size() - source_offset);
    std::copy_n(first.begin() + source_offset, count, target.begin());
    source_offset += count;
    co_return count;
  };
  TerminalTransferSourceBuffer buffer;
  std::array<std::uint8_t, 5> destination{};
  expect(co_await buffer.read_async(destination, first_fill, {}) == 5,
         "first read should return requested bytes");
  source_offset = 100;
  buffer.reset();
  expect(co_await buffer.read_async(destination, first_fill, {}) == 5,
         "read after seek should return requested bytes");
  expect(std::equal(destination.begin(), destination.end(), first.begin() + 100),
         "seek should discard stale prefetched bytes");

  buffer.reset();
  const std::array<std::uint8_t, 3> second{91, 92, 93};
  std::size_t second_offset = 0;
  auto second_fill = [&](std::span<std::uint8_t> target,
                         cardio::cancellation) -> cardio::promise<std::size_t> {
    const auto count = std::min(target.size(), second.size() - second_offset);
    std::copy_n(second.begin() + second_offset, count, target.begin());
    second_offset += count;
    co_return count;
  };
  expect(co_await buffer.read_async(destination, second_fill, {}) == 3,
         "next file should return its own bytes");
  expect(std::equal(destination.begin(), destination.begin() + 3, second.begin()),
         "next file must not expose the previous file's buffer");

  buffer.reset();
  source_offset = 0;
  expect(co_await buffer.read_async(destination, first_fill, {}) == 5,
         "cancellation setup should leave prefetched bytes");
  const auto fills_before_cancel = first_fill_count;
  cardio::cancellation_source stopped;
  stopped.cancel();
  bool cancelled = false;
  try {
    (void)co_await buffer.read_async(destination, first_fill,
                                     stopped.get_cancellation());
  } catch (const cardio::canceled_exception &) {
    cancelled = true;
  }
  expect(cancelled, "cancellation should stop reads with prefetched bytes");
  expect(first_fill_count == fills_before_cancel,
         "cancelled read should not refill the source");
  expect(co_await buffer.read_async(destination, first_fill, {}) == 5,
         "uncancelled read should keep prefetched bytes available");
  expect(std::equal(destination.begin(), destination.end(), first.begin() + 5),
         "cancelled read should not consume prefetched bytes");
}

static cardio::promise<void> run_tests(cardio::dispatcher_group_glib &group,
                                        std::exception_ptr &failure) {
  try {
    co_await test_buffered_reads();
    co_await test_short_read_and_empty_file();
    co_await test_seek_file_switch_and_cancellation();
  } catch (...) {
    failure = std::current_exception();
  }
  group.shutdown();
}

} // namespace elder_terms

int main() {
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib dispatcher(group);
  std::exception_ptr failure;
  auto task = elder_terms::run_tests(group, failure);
  dispatcher.park();
  elder_terms::expect(task.try_result(), "source buffer tests should complete");
  if (failure) std::rethrow_exception(failure);
  return 0;
}
