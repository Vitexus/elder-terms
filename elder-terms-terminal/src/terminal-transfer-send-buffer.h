// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <cardio.h>
#include <libxyzm/async.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace elder_terms {

/** Bounded byte-stream output for a single terminal transfer. */
struct TerminalTransferSendBuffer {
  /** Maximum number of pending wire bytes. */
  static constexpr std::size_t capacity = 64 * 1024;

  /** Discards pending bytes and sends later writes directly. */
  void abort() noexcept {
    used = 0;
    batch_budget_ms = 0;
    bypass = true;
  }

  /**
   * Accepts bytes into the bounded output stream.
   * @tparam Send Underlying partial-write callback.
   * @tparam Now Monotonic millisecond clock callback.
   * @param bytes Wire bytes offered by the protocol.
   * @param timeout_ms Time budget for the offered bytes, allocated in proportion
   * to the number accepted into this batch.
   * @param written_len Number of bytes accepted from this call.
   * @param send Underlying partial-write callback.
   * @param now Monotonic millisecond clock callback.
   * @param cancellation Transfer cancellation signal.
   * @return Completion after bytes are accepted and any full batch is written.
   * @remarks A short acceptance is allowed. After abort, writes bypass the buffer.
   */
  template <typename Send, typename Now>
  cardio::promise<void> send_async(std::span<const std::uint8_t> bytes,
      std::uint32_t timeout_ms, std::size_t &written_len, Send send, Now now,
      cardio::cancellation cancellation) {
    written_len = 0;
    if (bytes.empty()) co_return;
    try {
      cancellation.throw_if_cancellation_requested();
      if (bypass) {
        co_await send(bytes, timeout_ms, written_len, cancellation);
        if (written_len == 0 || written_len > bytes.size())
          throw xyzm_async_io_error("transfer send returned an invalid byte count");
        co_return;
      }

      const auto count = std::min(bytes.size(), capacity - used);
      std::copy_n(bytes.begin(), count, storage.begin() + used);
      used += count;
      const auto proportional = static_cast<std::uint64_t>(timeout_ms) * count;
      const auto contribution = proportional / bytes.size() +
                                (proportional % bytes.size() != 0);
      batch_budget_ms = static_cast<std::uint32_t>(std::min<std::uint64_t>(
          static_cast<std::uint64_t>(batch_budget_ms) + contribution,
          std::numeric_limits<std::uint32_t>::max()));
      if (used == capacity) co_await flush_async(send, now, cancellation);
      written_len = count;
    } catch (...) {
      abort();
      throw;
    }
  }

  /**
   * Writes all pending bytes to the underlying transport.
   * @tparam Send Underlying partial-write callback.
   * @tparam Now Monotonic millisecond clock callback.
   * @param send Underlying partial-write callback.
   * @param now Monotonic millisecond clock callback.
   * @param cancellation Transfer cancellation signal.
   * @return Completion after the entire pending batch is written.
   * @remarks One summed deadline covers all partial writes. Failure discards
   * the rest and switches subsequent writes to direct transport access.
   */
  template <typename Send, typename Now>
  cardio::promise<void> flush_async(Send send, Now now,
      cardio::cancellation cancellation) {
    try {
      cancellation.throw_if_cancellation_requested();
      if (bypass || used == 0) co_return;
      const auto started_ms = now();
      std::size_t sent = 0;
      while (sent < used) {
        cancellation.throw_if_cancellation_requested();
        const auto elapsed_ms = now() - started_ms;
        if (elapsed_ms >= batch_budget_ms)
          throw xyzm_async_timeout_error("transfer send buffer timed out");
        std::size_t written = 0;
        co_await send(std::span<const std::uint8_t>(storage).subspan(sent, used - sent),
                      batch_budget_ms - static_cast<std::uint32_t>(elapsed_ms),
                      written, cancellation);
        if (written == 0 || written > used - sent)
          throw xyzm_async_io_error("transfer send returned an invalid byte count");
        sent += written;
      }
      used = 0;
      batch_budget_ms = 0;
    } catch (...) {
      abort();
      throw;
    }
  }

private:
  std::array<std::uint8_t, capacity> storage{};
  std::size_t used = 0;
  std::uint32_t batch_budget_ms = 0;
  bool bypass = false;
};

} // namespace elder_terms
