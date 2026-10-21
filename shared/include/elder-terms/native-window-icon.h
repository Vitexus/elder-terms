#pragma once

#include <elder-terms/export.h>
#include <gtk/gtk.h>

namespace elder_terms {

/** Result of applying a connection icon to a native window. */
enum class NativeWindowIconStatus {
  /** Waiting for Wayland registry events or a GTK xdg_toplevel. */
  Pending,
  /** GTK sent the image list to its X11 window. */
  AppliedX11,
  /** An icon request was queued for the Wayland toplevel. */
  AppliedWayland,
  /** The active GDK display is neither X11 nor Wayland. */
  UnsupportedBackend,
  /** The active GTK runtime has no public Wayland toplevel accessor or signal. */
  UnsupportedGtk,
  /** The compositor does not offer the required icon or shared-memory protocol. */
  UnsupportedProtocol,
};

/**
 * Sets a native icon on a top-level window, retaining the images until the
 * window is destroyed or this function is called again.
 *
 * @param window GTK top-level window owned by the caller.
 * @param icons List of GdkPixbuf images; null restores the default icon.
 * @remarks Call on the GTK main thread. May be called before the window is realized. Wayland application
 * is asynchronous. The images are referenced, not copied; callers should not
 * modify their pixels after this call.
 */
ELDER_TERMS_API void set_native_window_icon(GtkWindow *window, GList *icons);

/**
 * Gets the latest native icon application status for a window.
 *
 * @param window GTK top-level window previously passed to set_native_window_icon.
 * @returns Current status; Pending if no icon has been requested yet.
 */
ELDER_TERMS_API NativeWindowIconStatus native_window_icon_status(GtkWindow *window);

} // namespace elder_terms
