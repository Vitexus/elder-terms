#include <elder-terms/connection-icon-image.h>
#include <elder-terms/connection-icon.h>
#include <algorithm>
#include <memory>
#include <utility>

namespace elder_terms {

struct ConnectionIconImageState {
  GtkImage *image;
  int size;
  std::string specification;
  std::shared_ptr<GdkPixbuf> pixels;
  std::string error;
  ConnectionIconStatusCallback status;
  cardio::cancellation_source cancellation;
  std::size_t generation = 0;
};

cairo_surface_t *connection_icon_surface(GdkPixbuf *image, int logical_size, int scale) {
  const auto fallback = image ? std::shared_ptr<GdkPixbuf>{} : default_connection_icon();
  if (!image) image = fallback.get();
  g_return_val_if_fail(image && logical_size > 0 && scale > 0, nullptr);
  auto *scaled = gdk_pixbuf_scale_simple(image, logical_size * scale,
      logical_size * scale, GDK_INTERP_BILINEAR);
  auto *surface = gdk_cairo_surface_create_from_pixbuf(scaled, scale, nullptr);
  g_object_unref(scaled);
  return surface;
}

static void render_connection_icon(ConnectionIconImageState *state) {
  if (!state->image || !state->pixels) return;
  auto *surface = connection_icon_surface(state->pixels.get(), state->size,
      gtk_widget_get_scale_factor(GTK_WIDGET(state->image)));
  gtk_image_set_from_surface(state->image, surface);
  cairo_surface_destroy(surface);
}

static cardio::promise<void> load_image_async(std::shared_ptr<ConnectionIconImageState> state,
    std::string specification, std::size_t generation, cardio::cancellation cancellation) {
  try {
    auto result = co_await load_connection_icon_async(std::move(specification), cancellation);
    if (!state->image || state->generation != generation) co_return;
    state->pixels = std::move(result.image);
    state->error = std::move(result.error);
    render_connection_icon(state.get());
    if (state->status) state->status(state->error);
  } catch (const cardio::canceled_exception &) {
    // A newer selection or widget destruction owns the presentation now.
  } catch (const std::exception &error) {
    if (!state->image || state->generation != generation) co_return;
    state->error = error.what();
    if (state->status) state->status(state->error);
  }
}

void set_connection_icon_image(GtkImage *image, const std::string &specification,
    int logical_size, ConnectionIconStatusCallback status) {
  g_return_if_fail(GTK_IS_IMAGE(image) && logical_size > 0);
  constexpr const char *key = "elder-terms-connection-icon-image";
  auto *holder = static_cast<std::shared_ptr<ConnectionIconImageState> *>(
      g_object_get_data(G_OBJECT(image), key));
  if (!holder) {
    holder = new std::shared_ptr<ConnectionIconImageState>(
        std::make_shared<ConnectionIconImageState>());
    (*holder)->image = image;
    g_object_set_data_full(G_OBJECT(image), key, holder, +[](gpointer data) {
      delete static_cast<std::shared_ptr<ConnectionIconImageState> *>(data);
    });
    g_signal_connect(image, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
      auto *state = static_cast<ConnectionIconImageState *>(data);
      state->image = nullptr;
      state->status = {};
      (void)state->cancellation.cancel();
    }), holder->get());
    g_signal_connect(image, "notify::scale-factor", G_CALLBACK(+[](GObject *, GParamSpec *, gpointer data) {
      render_connection_icon(static_cast<ConnectionIconImageState *>(data));
    }), holder->get());
  }
  const auto state = *holder;
  state->status = std::move(status);
  state->size = logical_size;
  if (state->specification == specification && state->pixels) {
    render_connection_icon(state.get());
    if (state->status) state->status(state->error);
    return;
  }
  (void)state->cancellation.cancel();
  state->cancellation = cardio::cancellation_source{};
  state->generation++;
  state->specification = specification;
  state->pixels = default_connection_icon();
  state->error.clear();
  render_connection_icon(state.get());
  if (state->status) state->status({});
  if (specification.empty() || specification == "default") return;
  cardio::fire_and_forget(load_image_async(state, specification, state->generation,
      state->cancellation.get_cancellation()));
}

} // namespace elder_terms
