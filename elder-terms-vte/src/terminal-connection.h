#pragma once

#include <boxrig/posix.h>
#include <cardio.h>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>
#include "terminal-transport.h"
#include "terminal-text-send.h"

namespace elder_terms {

/** Connection lifecycle, independent of terminal display ownership. */
enum class TerminalConnectionPhase {
  /** No command has been accepted yet. */
  idle,
  /** Parent preparation or child startup is pending. */
  starting,
  /** The child has successfully executed and the channel accepts input. */
  connected,
  /** Cancellation or failure is recovering the old channel. */
  stopping,
  /** The channel and child have been recovered; another start is permitted. */
  closed
};

/** An owned local command and bounded communication limits. */
struct TerminalLocalCommand {
  /** Executable followed by its arguments; execvp search semantics apply. */
  std::vector<std::string> arguments;
  /** TERM supplied only to the child process. */
  std::string terminal_type = "xterm-256color";
  /** Maximum bytes retained in each application queue, from 1 through 1 MiB. */
  std::size_t queue_capacity = 64 * 1024;
  /** Maximum bytes in one OS read or write, also bounded by queue capacity. */
  std::size_t io_chunk_size = 4096;
  /** iconv encoding at the peer boundary; the core always receives UTF-8. */
  std::string encoding = "UTF-8";
};

/** Snapshot separating spawn diagnostics, process exit, and resource recovery. */
struct TerminalConnectionState {
  /** Monotonically increasing accepted connection number. */
  std::uint64_t generation = 0;
  /** Current connection lifecycle. */
  TerminalConnectionPhase phase = TerminalConnectionPhase::idle;
  /** Parent preparation or child setup/exec diagnostic. */
  boxrig_posix_pty_spawn_result spawn{};
  /** Owned child PID, retained in the final report after reaping. */
  pid_t child = 0;
  /** Normal exit code, or -1 when unavailable. */
  int exit_code = -1;
  /** Terminating signal, or zero. */
  int signal_number = 0;
  /** Communication or observation errno, separate from spawn errno. */
  int operation_error = 0;
  /** Whether cancellation was requested for this generation. */
  bool cancelled = false;
  /** Whether the sole child reaper completed. */
  bool reaped = false;
  /** Largest observed application receive queue occupancy. */
  std::size_t receive_peak = 0;
  /** Largest observed application send queue occupancy. */
  std::size_t send_peak = 0;
  /** Currently applied peer encoding; empty before conversion is prepared. */
  std::string encoding;
};

/** Owns a boxrig session and its asynchronous transport on one dispatcher. */
struct TerminalConnection;

/**
 * Creates a communication owner without starting a command.
 * @param terminal Borrowed core, retained across connection generations.
 * @param wake Requests a future dispatch/frame; must not dispatch recursively.
 * @param output Receives each raw/cooked chunk once, before session retries.
 * @return Owned connection.
 * @remarks All functions and callbacks run on the core's Cardio dispatcher.
 * Cooked bytes may be empty for a partial encoded character; raw bytes may
 * be empty when finalizing the decoder at EOF or cancellation.
 */
TerminalConnection *create_terminal_connection(
    boxrig_terminal *terminal, std::function<void()> wake,
    std::function<void(std::span<const unsigned char>, std::span<const unsigned char>)> output);

/**
 * Destroys an inactive connection.
 * @param connection Owned connection, or nullptr.
 * @remarks Stop and await wait_stopped before destruction or core release.
 */
void destroy_terminal_connection(TerminalConnection *connection);

/**
 * Starts one local command using the currently confirmed core grid.
 * @param connection Inactive communication owner.
 * @param command Owned arguments and queue limits.
 * @return False while previous resources are still being recovered or limits are invalid.
 */
bool terminal_connection_start_local(TerminalConnection *connection, TerminalLocalCommand command);

/**
 * Starts a channel factory using the confirmed core grid and bounded queues.
 * @param connection Inactive communication owner.
 * @param request Owned factory and stream policy.
 * @return False if active, the factory is empty, or queue limits are invalid.
 */
bool terminal_connection_start_transport(TerminalConnection *connection, TerminalTransportRequest request);

/**
 * Cancels the current generation, including a pending synchronous spawn.
 * @param connection Communication owner.
 * @remarks Completion is reported only after asynchronous I/O and child recovery.
 */
void terminal_connection_stop(TerminalConnection *connection);

/**
 * Awaits recovery of the current operation without blocking the dispatcher.
 * @param connection Owner kept alive through completion.
 * @return Completion of all work borrowing the connection and core.
 */
cardio::promise<void> terminal_connection_wait_stopped(TerminalConnection *connection);

/**
 * Advances one bounded session dispatch after a readiness notification.
 * @param connection Communication owner.
 * @return True when another immediate dispatch can make progress.
 */
bool terminal_connection_dispatch(TerminalConnection *connection);

/**
 * Awaits one grid request at the peer before applying it to the core.
 * @param connection Communication owner.
 * @param rows Nonzero rows.
 * @param columns Nonzero columns.
 * @return False on transport or core failure; the connection then stops.
 */
cardio::promise<bool> terminal_connection_resize(TerminalConnection *connection, std::size_t rows, std::size_t columns);

/**
 * Exchanges the stream converters after draining already accepted output.
 * @param connection Connected owner, retained until completion.
 * @param encoding Desired iconv encoding name.
 * @return False when inactive, busy, cancelled, or the encoding is unavailable.
 * @remarks The caller inhibits user input first. The receive boundary is the
 * last completed transport read. Its partial character is finalized once;
 * neither display state nor connection generation is replaced.
 */
cardio::promise<bool> terminal_connection_change_encoding(TerminalConnection *connection, std::string encoding);

/**
 * Sends finite text after existing normal output has reached the peer.
 * @param connection Connected owner, retained until completion.
 * @param request Source, newline policy, rate, and progress/status callbacks.
 * @param cancellation Caller cancellation, combined with connection shutdown.
 * @return True only when every encoded payload byte has been sent.
 * @remarks The runtime owns input inhibition and active/finished callbacks.
 * Peer encoding is captured from the active connection. Normal receive and
 * logging continue; normal replies wait until this send finishes or cancels.
 */
cardio::promise<bool> terminal_connection_send_text(TerminalConnection *connection,
    TerminalTextSendRequest request, cardio::cancellation cancellation);

/**
 * Reserves binary receive routing before the runtime stops widget input.
 * @param connection Connected owner without another exclusive operation.
 * @return True when subsequent peer bytes belong to the bounded transfer queue.
 * @remarks Complete every accepted reservation with terminal_connection_transfer,
 * including cancellation. Normal output can drain while input stops.
 */
bool terminal_connection_prepare_transfer(TerminalConnection *connection);

/**
 * Runs a reserved binary transfer after user input is disabled.
 * @param connection Owner with an accepted transfer reservation.
 * @param request Protocol, files and presentation callbacks.
 * @param cancellation Caller cancellation, combined with connection shutdown.
 * @return True on complete transfer success.
 * @remarks Binary bytes bypass conversion, logs and the terminal parser.
 * Successful completion returns prefetched normal output to the same stream.
 * The runtime owns active/finished callbacks and input resumption.
 */
cardio::promise<bool> terminal_connection_transfer(TerminalConnection *connection,
    TerminalTransferRequest request, cardio::cancellation cancellation);

/**
 * Gets an observable lifecycle snapshot.
 * @param connection Communication owner.
 * @return Current or most recently completed generation's status.
 */
TerminalConnectionState terminal_connection_state(const TerminalConnection *connection);

}
