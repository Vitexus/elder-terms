// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <cardio.h>

namespace elder_terms {

/** Accepted control operations. */
enum class ControlCommand {
  /** Presents the launcher window. */
  open_application,
  /** Opens a saved connection by its current name. */
  open_connection,
  /** Opens a saved connection by its persistent UUID. */
  open_connection_id,
  /** Configures and reports global hotkeys. */
  setup,
};

/** A transport-independent request to control the launcher. */
struct ControlRequest {
  /** Requested operation. */
  ControlCommand command;
  /** Saved connection name or ID for the corresponding open command. */
  std::optional<std::string> connection;
  /** Desktop activation token, when supplied by the caller. */
  std::optional<std::string> activation_token;
};

/** Outcome of a control request. */
struct ControlReply {
  /** Whether the operation was accepted. */
  bool success;
  /** Whether setup should be queried again after initialization. */
  bool pending;
  /** Human-readable result or error. */
  std::string message;
};

/** Handles a control request on the dispatcher's thread. */
using ControlCallback = std::function<cardio::promise<ControlReply>(
    ControlRequest, cardio::cancellation)>;

/** Opaque asynchronous control server and its owned resources. */
struct ControlServerState;

/**
 * Starts a private Unix control socket on the current cardio dispatcher.
 * @param callback Runs accepted requests on the dispatcher's thread.
 * @returns Server owned by the caller.
 * @throws std::runtime_error on unsafe paths, an existing owner, or I/O errors.
 */
ControlServerState *create_control_server(ControlCallback callback);

/**
 * Cancels pending requests and releases the socket and process lock.
 * @param state Server to destroy, or null.
 */
void destroy_control_server(ControlServerState *state);

} // namespace elder_terms
