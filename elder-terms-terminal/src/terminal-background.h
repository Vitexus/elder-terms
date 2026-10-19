#pragma once

#include "main-window.h"

namespace elder_terms {
/** Application-owned background composition for one terminal window. */
struct TerminalBackground;
/** @param window Top-level GTK window, before realization. Selects an RGBA visual when available. */
void prepare_terminal_background_window(GtkWidget *window);
/** @param window Stable application window, borrowed through destruction. @returns Owned background state. */
TerminalBackground *create_terminal_background(MainWindow *window);
/** @param background Live state. Reattaches to the runtime's current widget and restores its preferences. */
void attach_terminal_background(TerminalBackground *background);
/** @param background Live state. @param settings Requested application settings.
 * @returns True after all native background settings have been accepted. */
bool apply_terminal_background(TerminalBackground *background, const SettingsStore &settings);
/** @param background Live state or null. @returns Actual desktop transparency status. */
std::string terminal_background_status(const TerminalBackground *background);
/** @param background Owned state or null; destroy before GTK objects. */
void destroy_terminal_background(TerminalBackground *background);
}
