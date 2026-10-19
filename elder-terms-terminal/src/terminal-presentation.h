#pragma once

#include "main-window.h"
#include "launch-options.h"

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
 * @return Whether the native cursor, font, history and grid preferences were accepted.
 */
bool apply_terminal_presentation(TerminalPresentation *presentation, const SettingsStore &settings);

/**
 * Observes native frame completion and publishes committed display settings.
 * @param presentation Live integration, called by the runtime change hook.
 */
void refresh_terminal_presentation(TerminalPresentation *presentation);

/**
 * Releases presentation sources and callbacks before GTK ownership is released.
 * @param presentation Owned integration, or nullptr.
 */
void destroy_terminal_presentation(TerminalPresentation *presentation);

}
