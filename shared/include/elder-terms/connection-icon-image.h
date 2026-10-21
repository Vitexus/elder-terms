#pragma once

#include <functional>
#include <string>
#include <gtk/gtk.h>
#include <elder-terms/export.h>

namespace elder_terms {

/** Receives an empty string on success or a diagnostic while using the default icon. */
using ConnectionIconStatusCallback = std::function<void(const std::string &)>;

/** Creates a scaled surface for a square connection icon.
 * @param image Borrowed square RGBA image, or null for the application icon.
 * @param logical_size Positive size in logical pixels.
 * @param scale Positive display scale factor.
 * @returns Owned surface, released with cairo_surface_destroy(). */
ELDER_TERMS_API cairo_surface_t *connection_icon_surface(
    GdkPixbuf *image, int logical_size, int scale);

/** Displays an icon and follows the image widget's display scale.
 * @param image Target widget; destruction cancels pending work.
 * @param specification Saved icon reference, including default or file:/absolute/path.
 * @param logical_size Positive size in logical pixels.
 * @param status Optional completion diagnostic, called on the GTK thread.
 * @remarks Call on the GTK thread with a cardio GLib dispatcher for file loading.
 * A later request supersedes earlier work. The widget owns its binding; callbacks
 * stop when the widget is destroyed. Missing images retain the specification and
 * display the application icon. This does not change native window icons. */
ELDER_TERMS_API void set_connection_icon_image(GtkImage *image,
    const std::string &specification, int logical_size,
    ConnectionIconStatusCallback status = {});

} // namespace elder_terms
