#pragma once

#include <functional>
#include <string>
#include <gtk/gtk.h>

namespace elder_terms {

// The relative widget owns the popover. Callbacks run after it is hidden.
GtkWidget *create_connection_icon_picker(GtkWidget *relative_to,
    const std::string &prefix, std::function<void(std::string)> selected,
    std::function<void()> choose_file);

/**
 * Restores the family and glyph color for the currently edited icon.
 *
 * @param popup Picker returned by create_connection_icon_picker.
 * @param specification Current connection icon setting.
 */
void sync_connection_icon_picker(GtkWidget *popup, const std::string &specification);

} // namespace elder_terms
