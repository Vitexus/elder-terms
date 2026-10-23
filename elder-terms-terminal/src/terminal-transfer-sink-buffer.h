// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <cardio.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace elder_terms {

/** Bounded payload buffering for the current receive-side file. */
struct TerminalTransferSinkBuffer {
  /** Maximum accepted payload retained before a file write. */
  static constexpr std::size_t capacity = 64 * 1024;

  /** Discards pending bytes when starting a new file. */
  void reset() noexcept { used = 0; }

  /**
   * Accepts payload bytes, writing full buffers to the sink.
   * @tparam Write Asynchronous callback that writes its entire input.
   * @param bytes Payload to accept.
   * @param write File write callback.
   * @param cancellation Cancellation signal.
   * @return Completion once all input has been accepted.
   */
  template <typename Write>
  cardio::promise<void> append_async(std::span<const std::uint8_t> bytes,
      Write write, cardio::cancellation cancellation) {
    cancellation.throw_if_cancellation_requested();
    while (!bytes.empty()) {
      cancellation.throw_if_cancellation_requested();
      const auto count = std::min(bytes.size(), capacity - used);
      std::copy_n(bytes.begin(), count, storage.begin() + used);
      used += count;
      bytes = bytes.subspan(count);
      if (used == capacity) co_await flush_async(write, cancellation);
    }
  }

  /**
   * Writes accepted bytes before closing the current file.
   * @tparam Write Asynchronous callback that writes its entire input.
   * @param write File write callback.
   * @param cancellation Cancellation signal for finalization.
   * @return Completion after pending bytes have been written.
   * @remarks A failed write discards its remainder to avoid duplicate retries.
   */
  template <typename Write>
  cardio::promise<void> flush_async(Write write, cardio::cancellation cancellation) {
    cancellation.throw_if_cancellation_requested();
    if (used == 0) co_return;
    const auto count = used;
    // A failed write can have consumed a prefix. Never replay it during cleanup.
    used = 0;
    co_await write(std::span<const std::uint8_t>(storage).first(count), cancellation);
  }

private:
  std::array<std::uint8_t, capacity> storage{};
  std::size_t used = 0;
};

} // namespace elder_terms
