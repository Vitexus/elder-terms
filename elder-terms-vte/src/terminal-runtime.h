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
 * Requests input inhibition or resumption while preserving selection and display.
 * @param runtime Live runtime.
 * @param enabled Whether application policy permits terminal input.
 * @remarks Connections additionally inhibit input while starting or closed.
 * The widget's input state distinguishes accepted STOPPING from DISABLED;
 * transport output may remain pending in either state.
 */
void terminal_runtime_request_input_enabled(TerminalRuntime *runtime, bool enabled);

}
