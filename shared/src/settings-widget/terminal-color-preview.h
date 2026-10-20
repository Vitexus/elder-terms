#pragma once
#include <gtk/gtk.h>
#include <elder-terms/settings.h>

namespace elder_terms {
/** Comparison samples owned by the settings editor; no connection or terminal I/O. */
struct TerminalColorPreview;
/** @param store Stable draft settings. @param prefix Accessible ID prefix.
 * @returns Owned comparison editor, released before the draft settings. */
TerminalColorPreview *create_terminal_color_preview(SettingsStore *store, const std::string &prefix);
/** @param preview Live comparison. @returns Expandable widget to place in the settings grid. */
GtkWidget *terminal_color_preview_root(TerminalColorPreview *preview);
/** @param preview Comparison or null. Refreshes both samples from the current draft. */
void sync_terminal_color_preview(TerminalColorPreview *preview);
/** @param preview Owned comparison or null. Disconnects its callbacks and releases widgets. */
void destroy_terminal_color_preview(TerminalColorPreview *preview);
}
