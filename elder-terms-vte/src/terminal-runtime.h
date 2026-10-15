#pragma once

#include <boxrig/gtk3.h>
#include <string_view>
#include <elder-terms/settings.h>
#include "terminal-connection.h"

namespace elder_terms {

/** Owns a terminal, its model, and the GTK attachment on the GTK thread. */
struct TerminalRuntime;

/**
 * Creates a terminal with OpenGL preferred rendering and the standard scroll container.
 * @param window Borrowed top-level window; must outlive the runtime.
 * @param content Borrowed content box, excluding client-side window decorations.
 * @return An owned runtime, or nullptr after a reported initialization failure.
 */
TerminalRuntime *create_terminal_runtime(GtkWindow *window, GtkBox *content);

/**
 * Cancels queued frames, destroys the GTK attachment, then releases model and core.
 * @param runtime Owned runtime, or nullptr.
 */
void destroy_terminal_runtime(TerminalRuntime *runtime);

/**
 * Gets the GTK terminal for native focus, accessibility, and portable UI operations.
 * @param runtime Live runtime.
 * @return Borrowed widget; owned by the runtime and its GTK container.
 */
BoxrigGtk3TerminalWidget *terminal_runtime_widget(TerminalRuntime *runtime);

/**
 * Feeds fixed UTF-8 driver output and schedules presentation.
 * @param runtime Live runtime.
 * @param text Complete, bounded terminal output.
 * @return Whether the complete output was consumed.
 * @remarks Streaming connections use a session transport, not this driver helper.
 */
bool terminal_runtime_feed(TerminalRuntime *runtime, std::string_view text);

/**
 * Requests a visual zoom step and schedules its layout transaction.
 * @param runtime Live runtime.
 * @param direction Requested visual step direction.
 * @return The widget's status; BUSY leaves the request unaccepted.
 */
boxrig_ui_status terminal_runtime_visual_zoom(TerminalRuntime *runtime, boxrig_font_zoom_direction direction);

/**
 * Reports whether the last frame completed its layout and presentation work.
 * @param runtime Live runtime.
 * @return False while native allocation, grid completion, or another frame is pending.
 */
bool terminal_runtime_is_settled(const TerminalRuntime *runtime);

/**
 * Installs a notification after frame processing, for application UI updates.
 * @param runtime Live runtime.
 * @param callback Called on the GTK thread; nullptr disables notification.
 * @param context Borrowed callback state, valid until removed.
 */
void terminal_runtime_set_changed_callback(
    TerminalRuntime *runtime, void (*callback)(void *), void *context);

/**
 * Starts a local command while retaining this runtime's display and history.
 * @param runtime Live runtime.
 * @param command Owned command and communication limits.
 * @return Whether a new connection generation was accepted.
 */
bool terminal_runtime_connect_local(TerminalRuntime *runtime, TerminalLocalCommand command);

/**
 * Starts a remote or serial channel after invalidating old widget input.
 * @param runtime Live runtime with no active connection.
 * @param request Reusable channel factory and stream policy.
 * @return Whether a new connection generation was accepted.
 */
bool terminal_runtime_connect_transport(TerminalRuntime *runtime, TerminalTransportRequest request);

/**
 * Requests a normal-state grid and schedules its layout work.
 * @param runtime Live runtime with an active widget.
 * @param rows Preferred nonzero rows.
 * @param columns Preferred nonzero columns.
 * @return Status of accepting the preference; peer completion is asynchronous.
 */
boxrig_ui_status terminal_runtime_set_preferred_grid(TerminalRuntime *runtime, std::size_t rows, std::size_t columns);

/**
 * Requests disconnection and waits for transport and child recovery.
 * @param runtime Runtime kept alive until completion.
 * @return Completion of all borrowed communication work.
 * @remarks Required before destroying a runtime that has started a connection.
 */
cardio::promise<void> terminal_runtime_disconnect(TerminalRuntime *runtime);

/**
 * Gets the current or completed connection snapshot.
 * @param runtime Live runtime.
 * @return Spawn, lifecycle, and child recovery information.
 */
TerminalConnectionState terminal_runtime_connection_state(const TerminalRuntime *runtime);

/**
 * Installs a receiver for normal output, before session parsing and retries.
 * @param runtime Live runtime.
 * @param callback Borrowed raw and UTF-8 bytes, valid during the call only.
 * @remarks Either span may be empty for an incomplete character or decoder
 * finalization; each byte in either stream is reported exactly once.
 */
void terminal_runtime_set_output_callback(TerminalRuntime *runtime,
    std::function<void(std::span<const unsigned char>, std::span<const unsigned char>)> callback);

/**
 * Requests an atomic exchange of Enter, Backspace and cursor-key settings.
 * @param runtime Live runtime.
 * @param settings Desired input settings; encoding belongs to the transport.
 * @remarks A BUSY widget retains its old settings until an input/frame
 * notification permits retry. A newer request replaces a pending request.
 */
void terminal_runtime_request_input_settings(TerminalRuntime *runtime, TerminalTextSettings settings);

/**
 * Reports whether an input-settings exchange is waiting for the widget.
 * @param runtime Live runtime.
 * @return True until a requested exchange is applied or fails.
 */
bool terminal_runtime_input_settings_pending(const TerminalRuntime *runtime);

/**
 * Requests a live encoding exchange after widget input has stopped.
 * @param runtime Connected runtime.
 * @param encoding Desired peer encoding.
 * @return False when inactive or an exchange is already running.
 * @remarks A pending request may be replaced before it starts. Invalid
 * encodings leave the active conversion and connection unchanged.
 */
bool terminal_runtime_request_encoding(TerminalRuntime *runtime, std::string encoding);

/**
 * Reports whether widget inhibition or stream exchange is still pending.
 * @param runtime Live runtime.
 * @return True until the requested encoding exchange finishes.
 */
bool terminal_runtime_encoding_pending(const TerminalRuntime *runtime);

/**
 * Starts a finite text send after standard widget input has stopped.
 * @param runtime Connected runtime without another exclusive operation.
 * @param request Text source, rate, newline policy and presentation callbacks.
 * @return True when the request is accepted.
 * @remarks Current key/newline settings and peer encoding are captured.
 * Display, selection, copying and received output remain available.
 */
bool terminal_runtime_start_text_send(TerminalRuntime *runtime, TerminalTextSendRequest request);

/**
 * Cancels a pending or active text send or binary transfer, then resumes input.
 * @param runtime Live runtime.
 * @return True when there was an operation to cancel.
 */
bool terminal_runtime_cancel_transfer(TerminalRuntime *runtime);

/**
 * Reserves binary receive routing, stops widget input, and runs X/Y/ZMODEM.
 * @param runtime Connected runtime without another exclusive operation.
 * @param request Protocol, local files and presentation callbacks.
 * @return True when the transfer reservation is accepted.
 * @remarks Accepted bytes remain in bounded queues while input stops and old
 * normal output drains. Display and selection remain available throughout.
 */
bool terminal_runtime_start_transfer(TerminalRuntime *runtime, TerminalTransferRequest request);

/**
 * Requests input inhibition or resumption while preserving selection and display.
 * @param runtime Live runtime.
 * @param enabled Whether application policy permits terminal input.
 * @remarks Connections additionally inhibit input while starting or closed.
 * The widget's input state distinguishes accepted STOPPING from DISABLED;
 * transport output may remain pending in either state.
 */
void terminal_runtime_request_input_enabled(TerminalRuntime *runtime, bool enabled);

}
