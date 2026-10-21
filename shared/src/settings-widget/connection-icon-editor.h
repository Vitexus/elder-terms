#pragma once

#include <functional>
#include <gtk/gtk.h>
#include <elder-terms/settings.h>

namespace elder_terms {
/** Connection-only icon controls owned by the settings editor. */
struct ConnectionIconEditor;
/** @param store Stable draft pointer. @param prefix Accessible widget ID prefix.
 * @param changed Draft notification. @returns Owned editor. */
ConnectionIconEditor *create_connection_icon_editor(SettingsStore *store,
    std::string prefix, std::function<void()> changed);
/** @param editor Live editor. @returns Widget owned by its GTK parent. */
GtkWidget *connection_icon_editor_root(ConnectionIconEditor *editor);
/** @param editor Editor or null. Synchronizes selection and preview from the draft. */
void sync_connection_icon_editor(ConnectionIconEditor *editor);
/** @param editor Owned editor or null. Closes its chooser and releases state. */
void destroy_connection_icon_editor(ConnectionIconEditor *editor);
} // namespace elder_terms
