// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "terminal-transfer.h"

namespace elder_terms {

/**
 * Incremental detector state for ZMODEM auto-start preambles.
 *
 * @remarks Only a short byte suffix is retained for detecting ZHEX headers
 * split across terminal backend reads.
 */
struct TerminalZmodemAutoStartDetectorState {
  /** Retained suffix from previous remote payloads. */
  std::string tail;
};

/** Separates ordinary output from the first complete, CRC-valid start header. */
struct TerminalZmodemAutoStartResult {
  /** Ordinary bytes before the header, or bytes ruled out as header prefixes. */
  std::string terminal_data;
  /** Complete header and subsequent bytes, to replay into the transfer. */
  std::string transfer_data;
  /** Local transfer direction, present only with a validated header. */
  std::optional<TerminalTransferDirection> direction;
};

/**
 * Holds a possible partial header and releases bytes as their routing is known.
 * @param state Incremental state, retaining at most 17 possible header bytes.
 * @param payload New raw peer bytes.
 * @return Ordinary output and, on detection, all accepted binary bytes.
 * @remarks Feed retained tail as ordinary output on EOF or when disabling
 * detection. Once a header is found, route subsequent input to the transfer.
 */
TerminalZmodemAutoStartResult filter_terminal_zmodem_auto_start(
    TerminalZmodemAutoStartDetectorState *state, std::string_view payload);

/**
 * Scans remote bytes for ZMODEM ZRQINIT/ZRINIT auto-start headers.
 *
 * @param state Incremental detector state updated with the newest payload.
 * @param payload Raw bytes received from the remote side.
 * @returns Transfer direction to start, or std::nullopt when no valid preamble
 * was found.
 */
std::optional<TerminalTransferDirection>
feed_terminal_zmodem_auto_start_detector(
    TerminalZmodemAutoStartDetectorState *state, std::string_view payload);

} // namespace elder_terms
