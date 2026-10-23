// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

#include <cardio.h>

namespace elder_terms {

/** Buffers file reads for a terminal transfer source. */
struct TerminalTransferSourceBuffer {
  /** Number of bytes requested for each underlying refill. */
  static constexpr std::size_t capacity = 64 * 1024;

  /** Discards prefetched bytes and EOF after seeking or switching files. */
  void reset() noexcept {
    next = 0;
    end = 0;
    eof = false;
  }

  /**
   * Reads available bytes, refilling from the underlying source only when empty.
   *
   * @tparam Fill Callback returning bytes placed into the supplied refill span.
   * @param destination Bytes returned to the protocol sender.
   * @param fill Underlying read callback; zero marks end of file.
   * @param cancellation Transfer cancellation signal.
   * @return Bytes returned, or zero at end of file.
   */
  template <typename Fill>
  cardio::promise<std::size_t> read_async(
      std::span<std::uint8_t> destination, Fill fill,
      cardio::cancellation cancellation) {
    cancellation.throw_if_cancellation_requested();
    if (destination.empty()) co_return 0;
    if (next == end && !eof) {
      const std::size_t count = co_await fill(storage, cancellation);
      if (count > storage.size())
        throw std::length_error("transfer source fill exceeded buffer capacity");
      next = 0;
      end = count;
      eof = count == 0;
    }
    cancellation.throw_if_cancellation_requested();
    const std::size_t count = std::min(destination.size(), end - next);
    std::copy_n(storage.begin() + next, count, destination.begin());
    next += count;
    co_return count;
  }

private:
  std::array<std::uint8_t, capacity> storage{};
  std::size_t next = 0;
  std::size_t end = 0;
  bool eof = false;
};

} // namespace elder_terms
