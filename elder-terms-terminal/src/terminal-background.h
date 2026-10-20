#pragma once

#include "main-window.h"
#include "terminal-background-image.h"
#include <boxrig/ui/background_effect.h>

namespace elder_terms {
/** Application-owned background composition for one terminal window. */
struct TerminalBackground;
/** @param window Top-level GTK window, before realization. Selects an RGBA visual when available. */
void prepare_terminal_background_window(GtkWidget *window);
/** @param window Stable application window, borrowed through destruction.
 * @param loader Image decoder dependency. @returns Owned background state. */
TerminalBackground *create_terminal_background(MainWindow *window,
    TerminalBackgroundImageLoader loader = load_terminal_background_image);
/** @param background Live state. Reattaches to the runtime's current widget and restores its preferences. */
void attach_terminal_background(TerminalBackground *background);
/** @param background Live state. @param settings Requested application settings.
 * @param image Prepared image or null to remove it.
 * @returns True after all native background settings have been accepted. */
bool apply_terminal_background(TerminalBackground *background, const SettingsStore &settings, TerminalBackgroundImagePtr image);
/** @param background Live state. @returns Shared image retained for rollback. */
TerminalBackgroundImagePtr terminal_background_image(const TerminalBackground *background);
/** Prepares image resources without changing presentation.
 * @param background Live state, which cancels preparation when destroyed.
 * @param settings Requested settings, copied before asynchronous work.
 * @param current Whether the requesting edit is still current; called on the UI thread.
 * @param complete Receives the prepared image or an error only for a still-current request.
 * @remarks Cached pixels are reused for opacity and placement changes. */
void prepare_terminal_background_image(TerminalBackground *background, const SettingsStore &settings,
    std::function<bool()> current, std::function<void(TerminalBackgroundImagePtr, std::string)> complete);
/** @param background Live state or null. Invalidates and cancels pending image preparation. */
void cancel_terminal_background_image(TerminalBackground *background);
/** @param background Live state or null. @returns Actual desktop transparency status. */
std::string terminal_background_status(const TerminalBackground *background);
/** Formats native acceptance separately from observed pixels.
 * @param mode Requested mode. @param transparent Actual desktop alpha support.
 * @param effect Latest compositor request state. @returns Localized actual status. */
std::string format_terminal_background_status(TerminalBackgroundMode mode, bool transparent,
    const boxrig_ui_background_effect_state &effect);
/** @param background Owned state or null; destroy before GTK objects. */
void destroy_terminal_background(TerminalBackground *background);
}
