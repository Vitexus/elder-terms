#include <elder-terms/native-window-icon.h>

#include <gdk/gdkx.h>
#include <gdk/gdkwayland.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>

static void check(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static unsigned long icon_pixel(GtkWidget *widget) {
  GdkWindow *gdk_window = gtk_widget_get_window(widget);
  Display *display = gdk_x11_display_get_xdisplay(gdk_window_get_display(gdk_window));
  Atom property = XInternAtom(display, "_NET_WM_ICON", False);
  Atom actual_type = None;
  int actual_format = 0;
  unsigned long count = 0;
  unsigned long remaining = 0;
  unsigned char *data = nullptr;
  int result = XGetWindowProperty(display, gdk_x11_window_get_xid(gdk_window),
                                  property, 0, 4, False, XA_CARDINAL,
                                  &actual_type, &actual_format, &count,
                                  &remaining, &data);
  check(result == Success && actual_type == XA_CARDINAL && actual_format == 32 &&
            count >= 3 && data != nullptr,
        "_NET_WM_ICON is missing");
  auto *values = reinterpret_cast<unsigned long *>(data);
  check(values[0] == 1 && values[1] == 1, "icon dimensions differ");
  unsigned long pixel = values[2] & 0xffffffffUL;
  XFree(data);
  return pixel;
}

static bool icon_property_exists(GtkWidget *widget) {
  GdkWindow *gdk_window = gtk_widget_get_window(widget);
  Display *display = gdk_x11_display_get_xdisplay(gdk_window_get_display(gdk_window));
  Atom property = XInternAtom(display, "_NET_WM_ICON", False);
  Atom type = None;
  int format = 0;
  unsigned long count = 0;
  unsigned long remaining = 0;
  unsigned char *data = nullptr;
  int result = XGetWindowProperty(display, gdk_x11_window_get_xid(gdk_window),
                                  property, 0, 1, False, XA_CARDINAL,
                                  &type, &format, &count, &remaining, &data);
  if (data != nullptr) {
    XFree(data);
  }
  return result == Success && type == XA_CARDINAL && count != 0;
}

static GdkPixbuf *solid_pixel(guchar red, guchar green, guchar blue) {
  GdkPixbuf *pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 1, 1);
  gdk_pixbuf_fill(pixbuf, (static_cast<guint32>(red) << 24) |
                             (static_cast<guint32>(green) << 16) |
                             (static_cast<guint32>(blue) << 8) | 0xff);
  return pixbuf;
}

struct WaylandTest {
  GtkWidget *widget = nullptr;
  int phase = 0;
  bool failed = false;
};

static gboolean recreate_wayland_widget(gpointer data) {
  auto *test = static_cast<WaylandTest *>(data);
  gtk_widget_hide(test->widget);
  gtk_widget_unrealize(test->widget);
  gtk_widget_show(test->widget);
  return G_SOURCE_REMOVE;
}

static void after_paint(GdkFrameClock *, WaylandTest *test) {
  auto *window = GTK_WINDOW(test->widget);
  auto status = elder_terms::native_window_icon_status(window);
  if (status == elder_terms::NativeWindowIconStatus::Pending) {
    return;
  }
  if (status != elder_terms::NativeWindowIconStatus::AppliedWayland) {
    test->failed = true;
    gtk_main_quit();
    return;
  }
  if (test->phase == 0) {
    GdkPixbuf *blue = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 8, 16);
    gdk_pixbuf_fill(blue, 0x1144ee80);
    GList *icons = g_list_append(nullptr, blue);
    elder_terms::set_native_window_icon(window, icons);
    g_list_free(icons);
    g_object_unref(blue);
  } else if (test->phase == 1) {
    elder_terms::set_native_window_icon(window, nullptr);
  } else if (test->phase == 2) {
    g_idle_add(recreate_wayland_widget, test);
    ++test->phase;
    return;
  } else if (test->phase == 3) {
    GdkPixbuf *red = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 16, 16);
    gdk_pixbuf_fill(red, 0xdd2233ff);
    GList *icons = g_list_append(nullptr, red);
    elder_terms::set_native_window_icon(window, icons);
    g_list_free(icons);
    g_object_unref(red);
  } else {
    gtk_main_quit();
    return;
  }
  ++test->phase;
}

static void on_wayland_widget_realize(GtkWidget *widget, WaylandTest *test) {
  GdkFrameClock *clock = gdk_window_get_frame_clock(gtk_widget_get_window(widget));
  g_signal_connect(clock, "after-paint", G_CALLBACK(after_paint), test);
}

static int run_wayland_test(elder_terms::NativeWindowIconStatus expected_initial_status) {
  if (!GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default())) {
    std::cerr << "Wayland display required\n";
    return 1;
  }
  GtkWidget *widget = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  GdkPixbuf *red = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 16, 16);
  gdk_pixbuf_fill(red, 0xdd2233ff);
  GList *icons = g_list_append(nullptr, red);
  elder_terms::set_native_window_icon(GTK_WINDOW(widget), icons);
  g_list_free(icons);
  g_object_unref(red);
  if (expected_initial_status != elder_terms::NativeWindowIconStatus::Pending) {
    gtk_widget_show(widget);
    while (elder_terms::native_window_icon_status(GTK_WINDOW(widget)) ==
           elder_terms::NativeWindowIconStatus::Pending) {
      g_main_context_iteration(nullptr, TRUE);
    }
    auto actual = elder_terms::native_window_icon_status(GTK_WINDOW(widget));
    bool correct = actual == expected_initial_status ||
                   (expected_initial_status == elder_terms::NativeWindowIconStatus::UnsupportedBackend &&
                    (actual == elder_terms::NativeWindowIconStatus::UnsupportedGtk ||
                     actual == elder_terms::NativeWindowIconStatus::UnsupportedProtocol));
    gtk_widget_destroy(widget);
    return correct ? 0 : 1;
  }
  WaylandTest test{widget};
  g_signal_connect(widget, "realize", G_CALLBACK(on_wayland_widget_realize), &test);
  gtk_widget_show(widget);
  gtk_widget_queue_draw(widget);
  gtk_main();
  gtk_widget_destroy(widget);
  return !test.failed && test.phase == 4 ? 0 : 1;
}

int main(int argc, char **argv) {
  try {
    check(gtk_init_check(&argc, &argv), "GTK initialization failed");
    if (argc > 1 && std::strcmp(argv[1], "wayland") == 0) {
      return run_wayland_test(elder_terms::NativeWindowIconStatus::Pending);
    }
    if (argc > 1 && std::strcmp(argv[1], "wayland-unsupported") == 0) {
      return run_wayland_test(elder_terms::NativeWindowIconStatus::UnsupportedGtk);
    }
    if (argc > 1 && std::strcmp(argv[1], "wayland-no-protocol") == 0) {
      return run_wayland_test(elder_terms::NativeWindowIconStatus::UnsupportedProtocol);
    }
    if (argc > 1 && std::strcmp(argv[1], "wayland-fallback") == 0) {
      return run_wayland_test(elder_terms::NativeWindowIconStatus::UnsupportedBackend);
    }
    check(GDK_IS_X11_DISPLAY(gdk_display_get_default()), "X11 test requires X11");
    GtkWidget *widget = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    auto *window = GTK_WINDOW(widget);
    GdkPixbuf *red = solid_pixel(0xdd, 0x22, 0x33);
    GList *icons = g_list_append(nullptr, red);
    elder_terms::set_native_window_icon(window, icons);
    g_list_free(icons);
    g_object_unref(red);
    gtk_widget_show(widget);
    gdk_display_sync(gdk_display_get_default());
    check(elder_terms::native_window_icon_status(window) ==
              elder_terms::NativeWindowIconStatus::AppliedX11,
          "X11 status differs");
    check(icon_pixel(widget) == 0xffdd2233UL, "first X11 icon pixel differs");
    GdkPixbuf *blue = solid_pixel(0x11, 0x44, 0xee);
    icons = g_list_append(nullptr, blue);
    elder_terms::set_native_window_icon(window, icons);
    g_list_free(icons);
    g_object_unref(blue);
    gdk_display_sync(gdk_display_get_default());
    check(icon_pixel(widget) == 0xff1144eeUL, "updated X11 icon pixel differs");
    elder_terms::set_native_window_icon(window, nullptr);
    gdk_display_sync(gdk_display_get_default());
    check(!icon_property_exists(widget), "X11 icon remains after reset");
    gtk_widget_hide(widget);
    gtk_widget_unrealize(widget);
    gtk_widget_show(widget);
    gdk_display_sync(gdk_display_get_default());
    check(!icon_property_exists(widget), "X11 icon returned after re-realization");
    red = solid_pixel(0xdd, 0x22, 0x33);
    icons = g_list_append(nullptr, red);
    elder_terms::set_native_window_icon(window, icons);
    g_list_free(icons);
    g_object_unref(red);
    gdk_display_sync(gdk_display_get_default());
    check(icon_pixel(widget) == 0xffdd2233UL,
          "X11 icon update failed after re-realization");
    gtk_widget_destroy(widget);
  } catch (const std::exception &exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
  return 0;
}
