#include <elder-terms/connection-icon-image.h>
#include <elder-terms/connection-icon-presets.h>
#include <elder-terms/connection-icon.h>
#include <elder-terms/native-window-icon.h>
#include <cardio.h>
#include <gdk/gdkx.h>
#include <glib/gstdio.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <cassert>
#include <functional>
#include <iostream>
#include <vector>

static std::vector<unsigned long> read_icon(GtkWidget *window) {
  auto *native = gtk_widget_get_window(window);
  auto *display = gdk_x11_display_get_xdisplay(gdk_window_get_display(native));
  Atom type = None;
  int format = 0;
  unsigned long count = 0, remaining = 0;
  unsigned char *bytes = nullptr;
  const auto result = XGetWindowProperty(display, gdk_x11_window_get_xid(native),
      XInternAtom(display, "_NET_WM_ICON", False), 0, 1024 * 1024, False,
      XA_CARDINAL, &type, &format, &count, &remaining, &bytes);
  assert(result == Success && type == XA_CARDINAL && format == 32 && !remaining);
  const auto *values = reinterpret_cast<unsigned long *>(bytes);
  std::vector<unsigned long> pixels(values, values + count);
  XFree(bytes);
  return pixels;
}

static unsigned long center(GtkWidget *window) {
  const auto pixels = read_icon(window);
  assert(pixels.size() >= 258 && pixels[0] == 16 && pixels[1] == 16);
  return pixels[2 + 8 * 16 + 8] & 0xffffffffUL;
}

static void wait_until(const std::function<bool()> &predicate) {
  bool expired = false;
  const auto timeout = g_timeout_add_seconds(10, +[](gpointer data) -> gboolean {
    *static_cast<bool *>(data) = true;
    return G_SOURCE_REMOVE;
  }, &expired);
  while (!predicate() && !expired) g_main_context_iteration(nullptr, TRUE);
  if (!expired) g_source_remove(timeout);
  assert(predicate());
}

static bool matches_full_image(GtkWidget *window, GdkPixbuf *expected) {
  const auto property = read_icon(window);
  bool checked_full_image = false;
  std::vector<unsigned long> sizes;
  for (std::size_t offset = 0; offset < property.size();) {
    const auto width = property[offset++];
    const auto height = property[offset++];
    assert(width == height && offset + width * height <= property.size());
    sizes.push_back(width);
    if (width == 256) {
      const auto *pixels = gdk_pixbuf_read_pixels(expected);
      const int stride = gdk_pixbuf_get_rowstride(expected);
      for (unsigned long y = 0; y < height; ++y) {
        for (unsigned long x = 0; x < width; ++x) {
          const auto *pixel = pixels + y * stride + x * 4;
          const unsigned long argb = (static_cast<unsigned long>(pixel[3]) << 24) |
              (pixel[0] << 16) | (pixel[1] << 8) | pixel[2];
          if ((property[offset + y * width + x] & 0xffffffffUL) != argb) return false;
        }
      }
      checked_full_image = true;
    }
    offset += width * height;
  }
  assert(checked_full_image);
  assert((sizes == std::vector<unsigned long>{16, 24, 32, 48, 64, 128, 256}));
  return true;
}

int main(int argc, char **argv) {
  using namespace elder_terms;
  gtk_init(&argc, &argv);
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  auto *directory = g_dir_make_tmp("elder-window-icon-XXXXXX", nullptr);
  assert(directory);
  const auto path = std::string(directory) + "/red.png";
  auto *red = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 256, 256);
  gdk_pixbuf_fill(red, 0xff0000ff);
  assert(gdk_pixbuf_save(red, path.c_str(), "png", nullptr, nullptr));
  auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  auto *image = gtk_image_new();
  gtk_container_add(GTK_CONTAINER(window), image);

  set_connection_window_icon(GTK_WINDOW(window), GTK_IMAGE(image), "default", 20);
  gtk_widget_show_all(window);
  assert(matches_full_image(window, default_connection_icon().get()));
  set_connection_window_icon(GTK_WINDOW(window), GTK_IMAGE(image), "file:" + path, 20);
  wait_until([&] { return center(window) == 0xffff0000UL; });
  assert(matches_full_image(window, red));
  assert(native_window_icon_status(GTK_WINDOW(window)) == NativeWindowIconStatus::AppliedX11);

  gtk_widget_hide(window);
  gtk_widget_unrealize(window);
  gtk_widget_show_all(window);
  assert(matches_full_image(window, red));
  set_connection_window_icon(GTK_WINDOW(window), GTK_IMAGE(image), "default", 20);
  assert(matches_full_image(window, default_connection_icon().get()));
  set_connection_window_icon(GTK_WINDOW(window), GTK_IMAGE(image), "file:" + path, 20);
  set_connection_window_icon(GTK_WINDOW(window), GTK_IMAGE(image), "material:action/home", 20);
  const auto home = connection_icon_preset_thumbnail("material:action/home", 256);
  wait_until([&] { return matches_full_image(window, home.get()); });

  // Pending file work must not keep a destroyed window alive or overwrite a newer choice.
  set_connection_window_icon(GTK_WINDOW(window), GTK_IMAGE(image), "file:" + path, 20);
  GWeakRef weak;
  g_weak_ref_init(&weak, window);
  gtk_widget_destroy(window);
  assert(g_weak_ref_get(&weak) == nullptr);
  g_weak_ref_clear(&weak);
  while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, FALSE);
  g_object_unref(red);
  g_remove(path.c_str());
  g_rmdir(directory);
  g_free(directory);
  std::cout << "PASS connection window icons, updates, re-realization and destruction\n";
}
