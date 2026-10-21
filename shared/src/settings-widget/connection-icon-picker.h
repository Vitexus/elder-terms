#pragma once

#include <functional>
#include <string>
#include <gtk/gtk.h>

namespace elder_terms {

// The relative widget owns the popover. Callbacks run after it is hidden.
GtkWidget *create_connection_icon_picker(GtkWidget *relative_to,
    const std::string &prefix, std::function<void(std::string)> selected,
    std::function<void()> choose_file);

} // namespace elder_terms
