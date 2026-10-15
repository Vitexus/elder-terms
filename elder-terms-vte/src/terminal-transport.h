#pragma once

#include <cardio.h>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace elder_terms {

class AuthenticatedSshTransport;

/**
 * Owned communication operations, independent of terminal parsing and widgets.
 * @remarks Callbacks retain their channel state. All operations run on one
 * dispatcher. Cancel and join reads, writes and resizes before calling close.
 */
struct TerminalTransport {
  /** Reads protocol-decoded peer bytes into the borrowed buffer; zero means EOF. */
  std::function<cardio::promise<std::size_t>(std::span<unsigned char>, cardio::cancellation)> read;
  /** Sends every borrowed peer-encoded byte, applying transport framing once. */
  std::function<cardio::promise<void>(std::span<const unsigned char>, cardio::cancellation)> write;
  /** Awaits the peer's grid request; arguments are rows, columns, cancellation. */
  std::function<cardio::promise<void>(std::size_t, std::size_t, cardio::cancellation)> resize;
  /** Stops and joins channel-owned work, including an optional SSH route. */
  std::function<cardio::promise<void>()> close;
  /** Returns retained protocol-decoded input after joining read, before close. */
  std::function<std::vector<unsigned char>()> finish_read = {};
  /** Sends BREAK and restores the line on cancellation; empty when unsupported. */
  std::function<cardio::promise<void>(cardio::cancellation)> send_break = {};
  /** Shares the current shell's authenticated SSH route; empty for other transports. */
  std::function<std::shared_ptr<AuthenticatedSshTransport>()> authenticated_ssh = {};
  /**
   * Optional carrier observer; false suspends sending without closing receive.
   * The first argument publishes changes; cancellation ends observation.
   * Release callbacks before returning. The connection joins this before close.
   */
  std::function<cardio::promise<void>(std::function<void(bool)>, cardio::cancellation)> watch_carrier = {};
};

/** A reusable channel factory and bounded stream-conversion policy. */
struct TerminalTransportRequest {
  /** Opens a fresh channel at the confirmed rows and columns, with cancellation. */
  std::function<cardio::promise<TerminalTransport>(std::size_t, std::size_t, cardio::cancellation)> open;
  /** Peer encoding; terminal parsing and generated input always use UTF-8. */
  std::string encoding = "UTF-8";
  /** Maximum bytes in each application queue, from 1 through 1 MiB. */
  std::size_t queue_capacity = 64 * 1024;
  /** Maximum source bytes converted per asynchronous operation. */
  std::size_t io_chunk_size = 4096;
  /**
   * Optional event-driven availability monitor, retained across generations.
   * The first argument notifies a possible device change; the second stops
   * monitoring. Return only after releasing callbacks. No timer retries are used.
   */
  std::function<cardio::promise<void>(std::function<void()>, cardio::cancellation)> watch_availability = {};
};

}
