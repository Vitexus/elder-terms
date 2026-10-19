#pragma once

#include <functional>
#include <gtk/gtk.h>
#include <elder-terms/settings.h>

namespace elder_terms {
/** Background controls owned by the parent settings editor. */
struct TerminalBackgroundEditor;
/** @param store Stable draft. @param prefix Accessible ID prefix. @param changed Draft/validation notification.
 * @returns Owned editor, released before its parent state. */
TerminalBackgroundEditor *create_terminal_background_editor(SettingsStore *store, std::string prefix,
    std::function<void()> changed);
/** @param editor Live editor. @returns Widget owned by its GTK parent. */
GtkWidget *terminal_background_editor_root(TerminalBackgroundEditor *editor);
/** @param editor Live editor or null. @param preserve_invalid Retain unfinished input on rebase. */
void sync_terminal_background_editor(TerminalBackgroundEditor *editor, bool preserve_invalid);
/** @param editor Editor or null. @returns Whether all edited background values can be applied. */
bool terminal_background_editor_is_valid(const TerminalBackgroundEditor *editor);
/** @param editor Live editor or null. @param text Actual background status; empty hides the label. */
void terminal_background_editor_set_status(TerminalBackgroundEditor *editor, const std::string &text);
/** @param editor Owned editor or null. */
void destroy_terminal_background_editor(TerminalBackgroundEditor *editor);
}
