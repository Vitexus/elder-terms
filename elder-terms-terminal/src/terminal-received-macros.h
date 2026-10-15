#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>
#include <elder-terms/settings/macro-settings.h>

namespace elder_terms {

/** Application effects emitted after one received-line match. */
struct TerminalReceivedMacroCallbacks {
  /** Normalizes one complete expanded send using the current input policy. */
  std::function<std::string(std::string)> prepare_send;
  /** Accepts a complete UTF-8 chunk, or returns false until output space is available. */
  std::function<bool(std::span<const unsigned char>)> send;
  /** Launches the expanded executable and arguments without a shell. */
  std::function<void(std::string, std::vector<std::string>)> command;
  /** Reports invalid rules, expansion failures, or matching errors. */
  std::function<void(std::string)> error;
};

/** Bounded progress from one macro dispatch. */
struct TerminalReceivedMacroProgress {
  /** More received data, matching work, or send chunks remain. */
  bool pending = false;
  /** A send needs output capacity; retry on a communication wakeup. */
  bool blocked = false;
  /** Decoded input retained in the finite notification queue. */
  std::size_t queued_bytes = 0;
  /** Bytes retained in the current logical line, at most 1 MiB. */
  std::size_t line_bytes = 0;
};

/** Owns ordered rules, received notifications, and at most one active matcher. */
struct TerminalReceivedMacros;

/**
 * Creates a received-line runner using boxrig's bounded regex language.
 * @param rules Ordered rules; only the first match may execute on each line.
 * @param callbacks Application effects, valid until the runner is destroyed.
 * @return Owned runner; invalid patterns are diagnosed and omitted.
 */
TerminalReceivedMacros *create_terminal_received_macros(
    std::vector<MacroRule> rules, TerminalReceivedMacroCallbacks callbacks);

/**
 * Retains one complete decoded receive notification in arrival order.
 * @param macros Live runner.
 * @param generation Monotonically increasing connection identity.
 * @param bytes Borrowed complete UTF-8, including control sequences.
 * @return False without accepting bytes if the 2 MiB notification queue is full.
 * @remarks New generations discard old work. Older-generation notifications
 * are ignored. The caller applies receive backpressure before queue exhaustion.
 */
bool terminal_received_macros_feed(TerminalReceivedMacros *macros,
    std::uint64_t generation, std::span<const unsigned char> bytes);

/**
 * Advances one line fragment, bounded matcher work, or one UTF-8 send chunk.
 * @param macros Live runner.
 * @param work_budget Positive maximum regex work in this dispatch.
 * @return Remaining work, output backpressure, and retained-byte observations.
 * @remarks Matching snapshots are never replaced by newer receive data. Each
 * line runs at most one action, before LF when a notification already matches.
 */
TerminalReceivedMacroProgress terminal_received_macros_dispatch(
    TerminalReceivedMacros *macros, std::size_t work_budget);

/**
 * Discards pending actions, matching, input, and the current line.
 * @param macros Live runner.
 * @param generation Connection identity associated with subsequent input.
 */
void terminal_received_macros_reset(TerminalReceivedMacros *macros, std::uint64_t generation);

/**
 * Releases a runner without invoking its callbacks.
 * @param macros Owned runner, or nullptr.
 */
void destroy_terminal_received_macros(TerminalReceivedMacros *macros);

}
