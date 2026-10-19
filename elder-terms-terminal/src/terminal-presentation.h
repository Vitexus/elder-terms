#pragma once

#include "main-window.h"
#include "launch-options.h"
#include "terminal-background-image.h"

namespace elder_terms {

/** Application preferences and shortcuts for one boxrig attachment. */
struct TerminalPresentation;

/**
 * Creates presentation integration without owning the runtime or GTK objects.
 * @param window Stable window state, retained until destruction.
 * @param options Optional deterministic UI fixture controls.
 * @param changed Receives committed grid, history capacity and DIP size.
 * @param send_break Requests the configured BREAK action.
 * @return Owned integration, released after runtime shutdown and before GTK ownership.
 */
TerminalPresentation *create_terminal_presentation(MainWindow *window, TestOptions options,
    std::function<void(TerminalDisplaySettings)> changed, std::function<void()> send_break);

/**
 * Attaches shortcuts and restores cursor preferences after widget reattachment.
 * @param presentation Live integration.
 */
void attach_terminal_presentation(TerminalPresentation *presentation);

/**
 * Routes Control+wheel steps to the native visual zoom sequence.
 * @param presentation Live integration.
 * @param event Normalized mouse event supplied by the native widget.
 * @return Application ownership for a zoom step, otherwise terminal routing.
 */
boxrig_input_mouse_route route_terminal_presentation_mouse(
    TerminalPresentation *presentation, const boxrig_mouse_event &event);

/**
 * Applies cursor presentation, direct font size, families, history, grid, chrome and shortcuts.
 * @param presentation Live integration.
 * @param settings Effective application settings.
 * @param image Prepared image, or null to clear it.
 * @return Whether the native cursor, font, history and grid preferences were accepted.
 */
bool apply_terminal_presentation(TerminalPresentation *presentation, const SettingsStore &settings, TerminalBackgroundImagePtr image);

/** @param presentation Live integration. @returns Retained image for rollback. */
TerminalBackgroundImagePtr terminal_presentation_background_image(const TerminalPresentation *presentation);
/** Prepares an image without changing the displayed terminal.
 * @param presentation Live integration; destruction cancels the request.
 * @param settings Requested preferences.
 * @param current Whether this request remains current.
 * @param complete Receives the image or error on the UI thread, possibly synchronously. */
void prepare_terminal_presentation_background(TerminalPresentation *presentation, const SettingsStore &settings,
    std::function<bool()> current, std::function<void(TerminalBackgroundImagePtr, std::string)> complete);
/** @param presentation Live integration or null. Cancels pending image preparation. */
void cancel_terminal_presentation_background(TerminalPresentation *presentation);

/**
 * Observes native frame completion and publishes committed display settings.
 * @param presentation Live integration, called by the runtime change hook.
 */
void refresh_terminal_presentation(TerminalPresentation *presentation);

/** @param presentation Live integration or null. @returns Actual background capabilities and fallback status. */
std::string terminal_presentation_background_status(const TerminalPresentation *presentation);

/**
 * Releases presentation sources and callbacks before GTK ownership is released.
 * @param presentation Owned integration, or nullptr.
 */
void destroy_terminal_presentation(TerminalPresentation *presentation);

}
