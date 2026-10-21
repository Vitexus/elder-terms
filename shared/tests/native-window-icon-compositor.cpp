#include "xdg-toplevel-icon-v1-server-protocol.h"
#include "native-xdg-shell-server-protocol.h"

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include <cassert>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

// An isolated compositor receives the real GTK client's requests. Its icon
// records are independent of the client's capability reporting and pixbufs.
struct IconServer;
struct IconSurface {
  IconServer *server = nullptr;
  wl_resource *surface = nullptr;
  wl_resource *xdg = nullptr;
  wl_resource *toplevel = nullptr;
  wl_resource *buffer = nullptr;
  std::vector<wl_resource *> callbacks;
  bool configured = false;
  bool icon_pending = false;
  std::uint32_t icon_id = 0;
};

struct IconServer {
  wl_display *display = nullptr;
  wl_resource *output = nullptr;
  std::uint32_t serial = 0;
  bool scale_after_first_icon_commit = false;
  bool scale_changed = false;
  std::vector<std::unique_ptr<IconSurface>> surfaces;
};

struct ReceivedIcon;
struct ReceivedBuffer {
  wl_listener destroyed{};
  bool *disconnecting = nullptr;
  wl_resource *resource = nullptr;
  int size = 0;
  int scale = 0;
  std::vector<std::uint32_t> pixels;
};

struct ReceivedIcon {
  wl_listener client_destroyed{};
  bool disconnecting = false;
  bool immutable = false;
  std::vector<std::unique_ptr<ReceivedBuffer>> buffers;
};

static void destroy_resource(wl_client *, wl_resource *resource) {
  wl_resource_destroy(resource);
}
static void no_request(wl_client *, wl_resource *) {}
static void no_integer(wl_client *, wl_resource *, std::int32_t) {}
static void no_unsigned(wl_client *, wl_resource *, std::uint32_t) {}
static void no_pair(wl_client *, wl_resource *, std::int32_t, std::int32_t) {}
static void no_rectangle(wl_client *, wl_resource *, std::int32_t,
                         std::int32_t, std::int32_t, std::int32_t) {}
static void no_object(wl_client *, wl_resource *, wl_resource *) {}
static void no_string(wl_client *, wl_resource *, const char *) {}
static void no_move(wl_client *, wl_resource *, wl_resource *, std::uint32_t) {}
static void no_resize(wl_client *, wl_resource *, wl_resource *, std::uint32_t,
                      std::uint32_t) {}
static void no_menu(wl_client *, wl_resource *, wl_resource *, std::uint32_t,
                    std::int32_t, std::int32_t) {}

static void attach(wl_client *, wl_resource *resource, wl_resource *buffer,
                   std::int32_t, std::int32_t) {
  static_cast<IconSurface *>(wl_resource_get_user_data(resource))->buffer = buffer;
}

static void frame(wl_client *client, wl_resource *resource, std::uint32_t id) {
  auto *surface = static_cast<IconSurface *>(wl_resource_get_user_data(resource));
  surface->callbacks.push_back(wl_resource_create(client, &wl_callback_interface, 1, id));
}

static void commit(wl_client *, wl_resource *resource) {
  auto *surface = static_cast<IconSurface *>(wl_resource_get_user_data(resource));
  if (surface->toplevel && !surface->configured) {
    wl_array states{};
    xdg_toplevel_send_configure(surface->toplevel, 240, 160, &states);
    xdg_surface_send_configure(surface->xdg, ++surface->server->serial);
    surface->configured = true;
    if (surface->server->output) wl_surface_send_enter(resource, surface->server->output);
  }
  if (surface->icon_pending) {
    std::cout << "COMMIT " << surface->icon_id << '\n';
    surface->icon_pending = false;
    if (surface->server->scale_after_first_icon_commit &&
        !surface->server->scale_changed && surface->server->output) {
      surface->server->scale_changed = true;
      wl_output_send_scale(surface->server->output, 2);
      wl_output_send_done(surface->server->output);
    }
  }
  if (surface->buffer) {
    wl_buffer_send_release(surface->buffer);
    surface->buffer = nullptr;
  }
  for (auto *callback : surface->callbacks) {
    wl_callback_send_done(callback, ++surface->server->serial);
    wl_resource_destroy(callback);
  }
  surface->callbacks.clear();
}

static const struct wl_surface_interface surface_implementation = {
    destroy_resource, attach, no_rectangle, frame, no_object, no_object,
    commit, no_integer, no_integer, no_rectangle, no_pair};
static const struct wl_region_interface region_implementation = {
    destroy_resource, no_rectangle, no_rectangle};

static void create_surface(wl_client *client, wl_resource *resource, std::uint32_t id) {
  auto *server = static_cast<IconServer *>(wl_resource_get_user_data(resource));
  auto surface = std::make_unique<IconSurface>();
  surface->server = server;
  surface->surface = wl_resource_create(client, &wl_surface_interface,
                                        wl_resource_get_version(resource), id);
  wl_resource_set_implementation(surface->surface, &surface_implementation,
                                 surface.get(), nullptr);
  server->surfaces.push_back(std::move(surface));
}

static void create_region(wl_client *client, wl_resource *, std::uint32_t id) {
  auto *region = wl_resource_create(client, &wl_region_interface, 1, id);
  wl_resource_set_implementation(region, &region_implementation, nullptr, nullptr);
}
static const struct wl_compositor_interface compositor_implementation = {create_surface, create_region};
static void bind_compositor(wl_client *client, void *data, std::uint32_t version, std::uint32_t id) {
  auto *resource = wl_resource_create(client, &wl_compositor_interface, version, id);
  wl_resource_set_implementation(resource, &compositor_implementation, data, nullptr);
}

static const struct wl_output_interface output_implementation = {destroy_resource};
static void bind_output(wl_client *client, void *data, std::uint32_t version, std::uint32_t id) {
  auto *server = static_cast<IconServer *>(data);
  server->output = wl_resource_create(client, &wl_output_interface, version, id);
  wl_resource_set_implementation(server->output, &output_implementation, data, nullptr);
  wl_output_send_geometry(server->output, 0, 0, 320, 200, WL_OUTPUT_SUBPIXEL_UNKNOWN,
                          "elder-terms", "Icon protocol test", WL_OUTPUT_TRANSFORM_NORMAL);
  wl_output_send_mode(server->output, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, 1280, 800, 60000);
  if (version >= 2) {
    wl_output_send_scale(server->output, 1);
    wl_output_send_done(server->output);
  }
}

static const struct wl_seat_interface seat_implementation = {nullptr, nullptr, nullptr, destroy_resource};
static void bind_seat(wl_client *client, void *, std::uint32_t version, std::uint32_t id) {
  auto *seat = wl_resource_create(client, &wl_seat_interface, version, id);
  wl_resource_set_implementation(seat, &seat_implementation, nullptr, nullptr);
  wl_seat_send_capabilities(seat, 0);
  if (version >= 2) wl_seat_send_name(seat, "icon-test-seat");
}

static void no_selection(wl_client *, wl_resource *, wl_resource *, std::uint32_t) {}
static void no_drag(wl_client *, wl_resource *, wl_resource *, wl_resource *, wl_resource *, std::uint32_t) {}
static const struct wl_data_source_interface source_implementation = {no_string, destroy_resource, no_unsigned};
static const struct wl_data_device_interface device_implementation = {no_drag, no_selection, destroy_resource};
static void create_source(wl_client *client, wl_resource *resource, std::uint32_t id) {
  auto *source = wl_resource_create(client, &wl_data_source_interface, wl_resource_get_version(resource), id);
  wl_resource_set_implementation(source, &source_implementation, nullptr, nullptr);
}
static void get_device(wl_client *client, wl_resource *resource, std::uint32_t id, wl_resource *) {
  auto *device = wl_resource_create(client, &wl_data_device_interface, wl_resource_get_version(resource), id);
  wl_resource_set_implementation(device, &device_implementation, nullptr, nullptr);
}
static const struct wl_data_device_manager_interface data_implementation = {create_source, get_device};
static void bind_data(wl_client *client, void *, std::uint32_t version, std::uint32_t id) {
  auto *resource = wl_resource_create(client, &wl_data_device_manager_interface, version, id);
  wl_resource_set_implementation(resource, &data_implementation, nullptr, nullptr);
}

static void record_application_id(wl_client *, wl_resource *, const char *id) {
  std::cout << "APP_ID " << id << std::endl;
}
static const struct xdg_toplevel_interface toplevel_implementation = {
    destroy_resource, no_object, no_string, record_application_id, no_menu, no_move, no_resize,
    no_pair, no_pair, no_request, no_request, no_object, no_request, no_request};
static void toplevel_destroyed(wl_resource *resource) {
  auto *surface = static_cast<IconSurface *>(wl_resource_get_user_data(resource));
  surface->toplevel = nullptr;
  surface->configured = false;
}
static void get_toplevel(wl_client *client, wl_resource *resource, std::uint32_t id) {
  auto *surface = static_cast<IconSurface *>(wl_resource_get_user_data(resource));
  surface->toplevel = wl_resource_create(client, &xdg_toplevel_interface, 1, id);
  wl_resource_set_implementation(surface->toplevel, &toplevel_implementation, surface, toplevel_destroyed);
}
static const struct xdg_surface_interface xdg_surface_implementation = {
    destroy_resource, get_toplevel, nullptr, no_rectangle, no_unsigned};
static void get_xdg_surface(wl_client *client, wl_resource *, std::uint32_t id, wl_resource *native) {
  auto *surface = static_cast<IconSurface *>(wl_resource_get_user_data(native));
  surface->xdg = wl_resource_create(client, &xdg_surface_interface, 1, id);
  wl_resource_set_implementation(surface->xdg, &xdg_surface_implementation, surface, nullptr);
}
static const struct xdg_positioner_interface positioner_implementation = {
    destroy_resource, no_pair, no_rectangle, no_unsigned, no_unsigned,
    no_unsigned, no_pair, no_request, no_pair, no_unsigned};
static void create_positioner(wl_client *client, wl_resource *, std::uint32_t id) {
  auto *positioner = wl_resource_create(client, &xdg_positioner_interface, 1, id);
  wl_resource_set_implementation(positioner, &positioner_implementation, nullptr, nullptr);
}
static const struct xdg_wm_base_interface wm_implementation = {
    destroy_resource, create_positioner, get_xdg_surface, no_unsigned};
static void bind_wm(wl_client *client, void *, std::uint32_t version, std::uint32_t id) {
  auto *wm = wl_resource_create(client, &xdg_wm_base_interface, version, id);
  wl_resource_set_implementation(wm, &wm_implementation, nullptr, nullptr);
}

static std::vector<std::uint32_t> read_pixels(wl_resource *resource) {
  auto *buffer = wl_shm_buffer_get(resource);
  assert(buffer);
  const int size = wl_shm_buffer_get_width(buffer);
  assert(size > 0 && size <= 1024 && size == wl_shm_buffer_get_height(buffer));
  assert(wl_shm_buffer_get_format(buffer) == WL_SHM_FORMAT_ARGB8888);
  const int stride = wl_shm_buffer_get_stride(buffer);
  std::vector<std::uint32_t> pixels(size * size);
  wl_shm_buffer_begin_access(buffer);
  const auto *bytes = static_cast<const std::uint8_t *>(wl_shm_buffer_get_data(buffer));
  for (int y = 0; y < size; ++y) std::memcpy(pixels.data() + y * size, bytes + y * stride, size * 4);
  wl_shm_buffer_end_access(buffer);
  return pixels;
}

static void icon_destroyed(wl_resource *resource) {
  auto *icon = static_cast<ReceivedIcon *>(wl_resource_get_user_data(resource));
  for (const auto &buffer : icon->buffers) {
    if (!buffer->resource) {
      assert(icon->disconnecting);
      continue;
    }
    // No buffer contents may change during the icon object's lifetime.
    assert(read_pixels(buffer->resource) == buffer->pixels);
    wl_list_remove(&buffer->destroyed.link);
  }
  wl_list_remove(&icon->client_destroyed.link);
  std::cout << "DESTROY " << wl_resource_get_id(resource) << '\n';
  delete icon;
}

static void icon_name(wl_client *, wl_resource *resource, const char *) {
  assert(!static_cast<ReceivedIcon *>(wl_resource_get_user_data(resource))->immutable);
}

static void add_buffer(wl_client *, wl_resource *resource, wl_resource *native, std::int32_t scale) {
  auto *icon = static_cast<ReceivedIcon *>(wl_resource_get_user_data(resource));
  assert(!icon->immutable && scale > 0);
  auto buffer = std::make_unique<ReceivedBuffer>();
  buffer->disconnecting = &icon->disconnecting;
  buffer->resource = native;
  buffer->size = wl_shm_buffer_get_width(wl_shm_buffer_get(native));
  buffer->scale = scale;
  buffer->pixels = read_pixels(native);
  for (const auto &previous : icon->buffers) {
    assert(previous->size != buffer->size || previous->scale != buffer->scale);
  }
  buffer->destroyed.notify = [](wl_listener *listener, void *) {
    auto *buffer = reinterpret_cast<ReceivedBuffer *>(listener);
    // Explicit requests must retain the buffer until icon destruction. On
    // disconnection libwayland destroys all resources in arbitrary order.
    assert(*buffer->disconnecting && "icon buffer destroyed too soon");
    buffer->resource = nullptr;
    wl_list_remove(&listener->link);
  };
  wl_resource_add_destroy_listener(native, &buffer->destroyed);
  std::uint64_t checksum = 0;
  for (auto pixel : buffer->pixels) checksum += pixel;
  std::cout << "BUFFER " << wl_resource_get_id(resource) << ' ' << buffer->size << ' '
            << scale << ' ' << buffer->pixels.front() << ' ' << checksum << '\n';
  icon->buffers.push_back(std::move(buffer));
}

static const struct xdg_toplevel_icon_v1_interface icon_implementation = {
    destroy_resource, icon_name, add_buffer};
static void create_icon(wl_client *client, wl_resource *, std::uint32_t id) {
  auto *resource = wl_resource_create(client, &xdg_toplevel_icon_v1_interface, 1, id);
  auto *icon = new ReceivedIcon{};
  icon->client_destroyed.notify = [](wl_listener *listener, void *) {
    auto *icon = reinterpret_cast<ReceivedIcon *>(listener);
    icon->disconnecting = true;
    wl_list_remove(&listener->link);
    wl_list_init(&listener->link);
  };
  wl_client_add_destroy_listener(client, &icon->client_destroyed);
  wl_resource_set_implementation(resource, &icon_implementation, icon, icon_destroyed);
}
static void set_icon(wl_client *, wl_resource *, wl_resource *toplevel, wl_resource *resource) {
  auto *surface = static_cast<IconSurface *>(wl_resource_get_user_data(toplevel));
  assert(surface && surface->toplevel == toplevel);
  if (resource) {
    auto *icon = static_cast<ReceivedIcon *>(wl_resource_get_user_data(resource));
    assert(!icon->buffers.empty());
    icon->immutable = true;
  }
  surface->icon_id = resource ? wl_resource_get_id(resource) : 0;
  surface->icon_pending = true;
  std::cout << "SET " << surface->icon_id << '\n';
}
static const struct xdg_toplevel_icon_manager_v1_interface manager_implementation = {
    destroy_resource, create_icon, set_icon};
static void bind_icons(wl_client *client, void *, std::uint32_t version, std::uint32_t id) {
  auto *manager = wl_resource_create(client, &xdg_toplevel_icon_manager_v1_interface, version, id);
  wl_resource_set_implementation(manager, &manager_implementation, nullptr, nullptr);
  xdg_toplevel_icon_manager_v1_send_icon_size(manager, 16);
  xdg_toplevel_icon_manager_v1_send_icon_size(manager, 32);
  xdg_toplevel_icon_manager_v1_send_icon_size(manager, 32);
  xdg_toplevel_icon_manager_v1_send_icon_size(manager, 4096);
  xdg_toplevel_icon_manager_v1_send_done(manager);
}

static int stop_server(int, void *data) {
  wl_display_terminate(static_cast<IconServer *>(data)->display);
  return 0;
}

int main(int argc, char **argv) {
  assert(argc == 3);
  std::cout << std::unitbuf;
  IconServer server;
  server.scale_after_first_icon_commit = std::strcmp(argv[2], "icons-scale") == 0;
  server.display = wl_display_create();
  assert(server.display && wl_display_init_shm(server.display) == 0);
  assert(wl_global_create(server.display, &wl_compositor_interface, 4, &server, bind_compositor));
  assert(wl_global_create(server.display, &wl_output_interface, 2, &server, bind_output));
  assert(wl_global_create(server.display, &wl_seat_interface, 5, &server, bind_seat));
  assert(wl_global_create(server.display, &wl_data_device_manager_interface, 3, &server, bind_data));
  assert(wl_global_create(server.display, &xdg_wm_base_interface, 1, &server, bind_wm));
  if (std::strcmp(argv[2], "icons") == 0 || server.scale_after_first_icon_commit)
    assert(wl_global_create(server.display, &xdg_toplevel_icon_manager_v1_interface, 1, &server, bind_icons));
  assert(wl_display_add_socket(server.display, argv[1]) == 0);
  auto *signal = wl_event_loop_add_signal(wl_display_get_event_loop(server.display), SIGTERM, stop_server, &server);
  assert(signal);
  std::cout << "READY\n";
  wl_display_run(server.display);
  wl_event_source_remove(signal);
  wl_display_destroy_clients(server.display);
  wl_display_destroy(server.display);
}
