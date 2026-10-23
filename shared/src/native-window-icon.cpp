// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/native-window-icon.h>

#include <gdk/gdkwayland.h>
#include <gdk/gdkx.h>
#include <gmodule.h>
#include <wayland-client.h>

#include "xdg-toplevel-icon-v1-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <linux/memfd.h>
#include <memory>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace elder_terms {

using GetXdgToplevel = xdg_toplevel *(*)(GdkWindow *);

struct NativeIconBuffer {
  wl_buffer *wayland_buffer = nullptr;
  void *pixels = MAP_FAILED;
  std::size_t byte_count = 0;
  int fd = -1;

  ~NativeIconBuffer() {
    if (wayland_buffer != nullptr) {
      wl_buffer_destroy(wayland_buffer);
    }
    if (pixels != MAP_FAILED) {
      munmap(pixels, byte_count);
    }
    if (fd >= 0) {
      close(fd);
    }
  }
};

struct NativeIcon {
  xdg_toplevel_icon_v1 *protocol_icon = nullptr;
  std::vector<std::unique_ptr<NativeIconBuffer>> buffers;

  ~NativeIcon() {
    if (protocol_icon != nullptr) {
      xdg_toplevel_icon_v1_destroy(protocol_icon);
    }
  }
};

struct NativeWindowIconState {
  GtkWindow *window = nullptr;
  GList *images = nullptr;
  NativeWindowIconStatus status = NativeWindowIconStatus::Pending;
  GdkWindow *gdk_window = nullptr;
  gulong widget_realize_handler = 0;
  gulong widget_unrealize_handler = 0;
  gulong widget_scale_handler = 0;
  gulong toplevel_handler = 0;
  GetXdgToplevel get_toplevel = nullptr;
  wl_display *wayland_display = nullptr;
  wl_registry *registry = nullptr;
  wl_callback *registry_sync = nullptr;
  wl_shm *shm = nullptr;
  xdg_toplevel_icon_manager_v1 *manager = nullptr;
  bool registry_complete = false;
  bool manager_complete = false;
  std::vector<int> preferred_sizes;
  std::unique_ptr<NativeIcon> icon;

  ~NativeWindowIconState() {
    if (gdk_window != nullptr && toplevel_handler != 0 &&
        g_signal_handler_is_connected(gdk_window, toplevel_handler)) {
      g_signal_handler_disconnect(gdk_window, toplevel_handler);
    }
    if (widget_realize_handler != 0 &&
        g_signal_handler_is_connected(window, widget_realize_handler)) {
      g_signal_handler_disconnect(window, widget_realize_handler);
    }
    if (widget_unrealize_handler != 0 &&
        g_signal_handler_is_connected(window, widget_unrealize_handler)) {
      g_signal_handler_disconnect(window, widget_unrealize_handler);
    }
    if (widget_scale_handler != 0 &&
        g_signal_handler_is_connected(window, widget_scale_handler)) {
      g_signal_handler_disconnect(window, widget_scale_handler);
    }
    icon.reset();
    if (registry_sync != nullptr) {
      wl_callback_destroy(registry_sync);
    }
    if (manager != nullptr) {
      xdg_toplevel_icon_manager_v1_destroy(manager);
    }
    if (shm != nullptr) {
      wl_shm_destroy(shm);
    }
    if (registry != nullptr) {
      wl_registry_destroy(registry);
    }
    if (wayland_display != nullptr) {
      wl_display_flush(wayland_display);
    }
    g_list_free_full(images, g_object_unref);
  }
};

constexpr char state_key[] = "elder-terms-native-window-icon-state";

static GetXdgToplevel lookup_get_toplevel() {
  GModule *module = g_module_open(nullptr, G_MODULE_BIND_LAZY);
  if (module == nullptr) {
    return nullptr;
  }
  gpointer symbol = nullptr;
  bool found = g_module_symbol(module, "gdk_wayland_window_get_xdg_toplevel", &symbol);
  g_module_close(module);
  return found && symbol != nullptr ? reinterpret_cast<GetXdgToplevel>(symbol) : nullptr;
}

static int create_shared_file(std::size_t count) {
  int fd = memfd_create("elder-terms-icon", MFD_CLOEXEC);
  if (fd < 0) {
    return -1;
  }
  if (ftruncate(fd, static_cast<off_t>(count)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static std::unique_ptr<NativeIconBuffer> make_buffer(wl_shm *shm, GdkPixbuf *pixbuf) {
  const int width = gdk_pixbuf_get_width(pixbuf);
  const int height = gdk_pixbuf_get_height(pixbuf);
  const int side = std::max(width, height);
  if (side <= 0 || side > 1024) {
    return nullptr;
  }
  const std::size_t stride = static_cast<std::size_t>(side) * 4;
  const std::size_t count = stride * static_cast<std::size_t>(side);
  auto buffer = std::make_unique<NativeIconBuffer>();
  buffer->fd = create_shared_file(count);
  if (buffer->fd < 0) {
    return nullptr;
  }
  buffer->pixels = mmap(nullptr, count, PROT_READ | PROT_WRITE, MAP_SHARED, buffer->fd, 0);
  if (buffer->pixels == MAP_FAILED) {
    return nullptr;
  }
  buffer->byte_count = count;
  std::memset(buffer->pixels, 0, count);
  const int components = gdk_pixbuf_get_n_channels(pixbuf);
  const int source_stride = gdk_pixbuf_get_rowstride(pixbuf);
  const guchar *source = gdk_pixbuf_read_pixels(pixbuf);
  auto *destination = static_cast<std::uint32_t *>(buffer->pixels);
  const int offset_x = (side - width) / 2;
  const int offset_y = (side - height) / 2;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const guchar *pixel = source + y * source_stride + x * components;
      const std::uint32_t alpha = components == 4 ? pixel[3] : 255;
      const std::uint32_t red = (pixel[0] * alpha + 127) / 255;
      const std::uint32_t green = (pixel[1] * alpha + 127) / 255;
      const std::uint32_t blue = (pixel[2] * alpha + 127) / 255;
      destination[(y + offset_y) * side + x + offset_x] =
          (alpha << 24) | (red << 16) | (green << 8) | blue;
    }
  }
  wl_shm_pool *pool = wl_shm_create_pool(shm, buffer->fd, static_cast<int>(count));
  if (pool == nullptr) {
    return nullptr;
  }
  buffer->wayland_buffer = wl_shm_pool_create_buffer(
      pool, 0, side, side, static_cast<int>(stride), WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);
  if (buffer->wayland_buffer == nullptr) {
    return nullptr;
  }
  return buffer;
}

static std::unique_ptr<NativeIconBuffer> make_scaled_buffer(
    wl_shm *shm, GdkPixbuf *source, int edge) {
  const int width = gdk_pixbuf_get_width(source);
  const int height = gdk_pixbuf_get_height(source);
  if (edge <= 0 || edge > 1024 || width <= 0 || height <= 0) return nullptr;
  const double factor = static_cast<double>(edge) /
                        static_cast<double>(std::max(width, height));
  const int scaled_width = std::max(1, static_cast<int>(std::round(width * factor)));
  const int scaled_height = std::max(1, static_cast<int>(std::round(height * factor)));
  if (scaled_width == width && scaled_height == height) return make_buffer(shm, source);
  auto *scaled = gdk_pixbuf_scale_simple(source, scaled_width, scaled_height,
                                        GDK_INTERP_BILINEAR);
  if (scaled == nullptr) return nullptr;
  auto buffer = make_buffer(shm, scaled);
  g_object_unref(scaled);
  return buffer;
}

static GdkPixbuf *closest_image(GList *images, int edge) {
  GdkPixbuf *selected = nullptr;
  int selected_side = 0;
  for (GList *item = images; item != nullptr; item = item->next) {
    auto *candidate = GDK_PIXBUF(item->data);
    const int side = std::max(gdk_pixbuf_get_width(candidate),
                              gdk_pixbuf_get_height(candidate));
    if (side <= 0) continue;
    // Prefer downscaling the smallest suitable source over enlarging a small
    // raster. If none is large enough, enlarge the largest available source.
    if (selected == nullptr ||
        (side >= edge && (selected_side < edge || side < selected_side)) ||
        (side < edge && selected_side < edge && side > selected_side)) {
      selected = candidate;
      selected_side = side;
    }
  }
  return selected;
}

static void apply_wayland_icon(NativeWindowIconState *state) {
  if (!state->registry_complete || !state->manager_complete ||
      state->manager == nullptr || state->shm == nullptr || state->gdk_window == nullptr) {
    return;
  }
  xdg_toplevel *toplevel = state->get_toplevel(state->gdk_window);
  if (toplevel == nullptr) {
    state->status = NativeWindowIconStatus::Pending;
    return;
  }
  if (state->images == nullptr) {
    xdg_toplevel_icon_manager_v1_set_icon(state->manager, toplevel, nullptr);
    state->icon.reset();
    state->status = NativeWindowIconStatus::AppliedWayland;
    gtk_widget_queue_draw(GTK_WIDGET(state->window));
    return;
  }
  auto icon = std::make_unique<NativeIcon>();
  icon->protocol_icon = xdg_toplevel_icon_manager_v1_create_icon(state->manager);
  if (icon->protocol_icon == nullptr) {
    state->status = NativeWindowIconStatus::UnsupportedProtocol;
    return;
  }
  const int scale = std::max(1, gtk_widget_get_scale_factor(GTK_WIDGET(state->window)));
  std::vector<int> logical_sizes = state->preferred_sizes;
  for (GList *item = state->images; item != nullptr; item = item->next) {
    auto *image = GDK_PIXBUF(item->data);
    logical_sizes.push_back(std::max(gdk_pixbuf_get_width(image),
                                     gdk_pixbuf_get_height(image)));
  }
  logical_sizes.push_back(256);
  std::sort(logical_sizes.begin(), logical_sizes.end());
  logical_sizes.erase(std::unique(logical_sizes.begin(), logical_sizes.end()),
                      logical_sizes.end());
  for (const int logical_size : logical_sizes) {
    if (logical_size <= 0 || logical_size > 1024 / scale) continue;
    const int edge = logical_size * scale;
    auto *source = closest_image(state->images, edge);
    if (source == nullptr) continue;
    auto buffer = make_scaled_buffer(state->shm, source, edge);
    if (buffer != nullptr) {
      xdg_toplevel_icon_v1_add_buffer(icon->protocol_icon, buffer->wayland_buffer, scale);
      icon->buffers.push_back(std::move(buffer));
    }
  }
  if (icon->buffers.empty()) {
    state->status = NativeWindowIconStatus::UnsupportedProtocol;
    return;
  }
  xdg_toplevel_icon_manager_v1_set_icon(state->manager, toplevel, icon->protocol_icon);
  state->icon = std::move(icon);
  state->status = NativeWindowIconStatus::AppliedWayland;
  // The request is double-buffered; GTK owns the surface and commits it on
  // its next paint. Queue a paint without committing GTK's surface ourselves.
  gtk_widget_queue_draw(GTK_WIDGET(state->window));
}

static void on_manager_icon_size(void *data, xdg_toplevel_icon_manager_v1 *, int32_t size) {
  auto *state = static_cast<NativeWindowIconState *>(data);
  if (size > 0 && size <= 1024 &&
      std::find(state->preferred_sizes.begin(), state->preferred_sizes.end(), size) ==
          state->preferred_sizes.end()) {
    state->preferred_sizes.push_back(size);
  }
}

static void on_manager_done(void *data, xdg_toplevel_icon_manager_v1 *) {
  auto *state = static_cast<NativeWindowIconState *>(data);
  state->manager_complete = true;
  apply_wayland_icon(state);
}

static const xdg_toplevel_icon_manager_v1_listener manager_listener{
    on_manager_icon_size, on_manager_done};

static void on_shm_format(void *, wl_shm *, uint32_t) {}

static const wl_shm_listener shm_listener{on_shm_format};

static void on_registry_global(void *data, wl_registry *registry, uint32_t name,
                        const char *interface, uint32_t version) {
  auto *state = static_cast<NativeWindowIconState *>(data);
  if (std::strcmp(interface, xdg_toplevel_icon_manager_v1_interface.name) == 0 &&
      state->manager == nullptr) {
    state->manager = static_cast<xdg_toplevel_icon_manager_v1 *>(
        wl_registry_bind(registry, name, &xdg_toplevel_icon_manager_v1_interface,
                         std::min(version, 1u)));
    if (state->manager != nullptr) {
      xdg_toplevel_icon_manager_v1_add_listener(state->manager, &manager_listener, state);
    }
  } else if (std::strcmp(interface, wl_shm_interface.name) == 0 && state->shm == nullptr) {
    state->shm = static_cast<wl_shm *>(
        wl_registry_bind(registry, name, &wl_shm_interface, std::min(version, 1u)));
    if (state->shm != nullptr) {
      wl_shm_add_listener(state->shm, &shm_listener, state);
    }
  }
}

static void on_registry_remove(void *, wl_registry *, uint32_t) {}

const wl_registry_listener registry_listener{on_registry_global, on_registry_remove};

static void on_registry_sync_done(void *data, wl_callback *callback, uint32_t) {
  auto *state = static_cast<NativeWindowIconState *>(data);
  state->registry_sync = nullptr;
  wl_callback_destroy(callback);
  state->registry_complete = true;
  if (state->manager == nullptr || state->shm == nullptr) {
    state->status = NativeWindowIconStatus::UnsupportedProtocol;
  } else {
    apply_wayland_icon(state);
  }
}

static const wl_callback_listener registry_sync_listener{on_registry_sync_done};

static void on_toplevel_realized(GdkWindow *, NativeWindowIconState *state) {
  apply_wayland_icon(state);
}

static void on_widget_unrealize(GtkWidget *, NativeWindowIconState *state) {
  if (state->gdk_window != nullptr && state->toplevel_handler != 0 &&
      g_signal_handler_is_connected(state->gdk_window, state->toplevel_handler)) {
    g_signal_handler_disconnect(state->gdk_window, state->toplevel_handler);
  }
  state->gdk_window = nullptr;
  state->toplevel_handler = 0;
  state->icon.reset();
  if (state->status == NativeWindowIconStatus::AppliedWayland) {
    state->status = NativeWindowIconStatus::Pending;
  }
}

static void on_widget_realize(GtkWidget *widget, NativeWindowIconState *state) {
  state->gdk_window = gtk_widget_get_window(widget);
  if (state->get_toplevel != nullptr && state->gdk_window != nullptr) {
    state->toplevel_handler = g_signal_connect(state->gdk_window,
                                                "xdg-toplevel-realized",
                                                G_CALLBACK(on_toplevel_realized), state);
    apply_wayland_icon(state);
  }
}

static void on_widget_scale(GObject *, GParamSpec *, NativeWindowIconState *state) {
  apply_wayland_icon(state);
}

static void initialize_wayland(NativeWindowIconState *state, GdkDisplay *display) {
  state->get_toplevel = lookup_get_toplevel();
  g_type_ensure(GDK_TYPE_WAYLAND_WINDOW);
  if (state->get_toplevel == nullptr ||
      g_signal_lookup("xdg-toplevel-realized", GDK_TYPE_WAYLAND_WINDOW) == 0) {
    state->status = NativeWindowIconStatus::UnsupportedGtk;
    return;
  }
  state->wayland_display = gdk_wayland_display_get_wl_display(display);
  wl_display *wayland_display = state->wayland_display;
  state->registry = wl_display_get_registry(wayland_display);
  if (state->registry == nullptr ||
      wl_registry_add_listener(state->registry, &registry_listener, state) != 0) {
    state->status = NativeWindowIconStatus::UnsupportedProtocol;
    return;
  }
  state->registry_sync = wl_display_sync(wayland_display);
  if (state->registry_sync == nullptr ||
      wl_callback_add_listener(state->registry_sync, &registry_sync_listener, state) != 0) {
    state->status = NativeWindowIconStatus::UnsupportedProtocol;
    return;
  }
  wl_display_flush(wayland_display);
  state->widget_realize_handler = g_signal_connect(
      state->window, "realize", G_CALLBACK(on_widget_realize), state);
  state->widget_unrealize_handler = g_signal_connect(
      state->window, "unrealize", G_CALLBACK(on_widget_unrealize), state);
  state->widget_scale_handler = g_signal_connect(
      state->window, "notify::scale-factor", G_CALLBACK(on_widget_scale), state);
  if (gtk_widget_get_realized(GTK_WIDGET(state->window))) {
    on_widget_realize(GTK_WIDGET(state->window), state);
  }
}

static void destroy_state(gpointer data) { delete static_cast<NativeWindowIconState *>(data); }

void set_native_window_icon(GtkWindow *window, GList *icons) {
  g_return_if_fail(GTK_IS_WINDOW(window));
  auto *state = static_cast<NativeWindowIconState *>(g_object_get_data(G_OBJECT(window), state_key));
  if (state == nullptr) {
    state = new NativeWindowIconState();
    state->window = window;
    g_object_set_data_full(G_OBJECT(window), state_key, state, destroy_state);
    GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(window));
    if (GDK_IS_WAYLAND_DISPLAY(display)) {
      initialize_wayland(state, display);
    } else if (!GDK_IS_X11_DISPLAY(display)) {
      state->status = NativeWindowIconStatus::UnsupportedBackend;
    }
  }
  g_list_free_full(state->images, g_object_unref);
  state->images = nullptr;
  for (GList *item = icons; item != nullptr; item = item->next) {
    if (GDK_IS_PIXBUF(item->data)) {
      state->images = g_list_append(state->images, g_object_ref(item->data));
    }
  }
  GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(window));
  if (GDK_IS_X11_DISPLAY(display)) {
    gtk_window_set_icon_list(window, state->images);
    state->status = NativeWindowIconStatus::AppliedX11;
  } else if (GDK_IS_WAYLAND_DISPLAY(display) &&
             state->status != NativeWindowIconStatus::UnsupportedGtk &&
             state->status != NativeWindowIconStatus::UnsupportedProtocol) {
    apply_wayland_icon(state);
  }
}

NativeWindowIconStatus native_window_icon_status(GtkWindow *window) {
  g_return_val_if_fail(GTK_IS_WINDOW(window), NativeWindowIconStatus::UnsupportedBackend);
  auto *state = static_cast<NativeWindowIconState *>(g_object_get_data(G_OBJECT(window), state_key));
  return state != nullptr ? state->status : NativeWindowIconStatus::Pending;
}

} // namespace elder_terms
