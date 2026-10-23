// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

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

/** Displays a connection icon and publishes the same image to its native window.
 * @param window Target window, held weakly; native support is detected at runtime.
 * @param image Title bar image; destruction cancels pending loading.
 * @param specification Saved icon reference, including default, file or preset IDs.
 * @param logical_size Positive title bar size in logical pixels.
 * @remarks Call on the GTK thread with a cardio GLib dispatcher for file loading.
 * One load supplies both displays. Unsupported native backends keep the title bar
 * image working. Later calls supersede pending work, and missing files use the
 * application image. The binding does not change the process-wide default icon. */
ELDER_TERMS_API void set_connection_window_icon(GtkWindow *window, GtkImage *image,
    const std::string &specification, int logical_size);

} // namespace elder_terms
