#include "terminal-background.h"
#include "terminal-runtime.h"
#include <vector>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

struct TerminalBackground {
  MainWindow *window;
  GtkCssProvider *provider;
  GdkScreen *screen;
  std::vector<GtkWidget *> paths;
  TerminalBackgroundSettings settings{TerminalBackgroundMode::opaque, 80, {0, 0, 0}, 0};
  RgbColor backing{0, 0, 0};
  bool desktop_transparent = false;
};

static void select_visual(GtkWidget *window, GdkScreen *, gpointer) {
  if (gtk_widget_get_realized(window)) return;
  if (auto *visual = gdk_screen_get_rgba_visual(gtk_widget_get_screen(window)))
    gtk_widget_set_visual(window, visual);
}

void prepare_terminal_background_window(GtkWidget *window) {
  select_visual(window, nullptr, nullptr);
  g_signal_connect(window, "screen-changed", G_CALLBACK(select_visual), nullptr);
  gtk_widget_set_app_paintable(window, TRUE);
  g_signal_connect(window, "draw", G_CALLBACK(+[](GtkWidget *, cairo_t *cr, gpointer) {
    // Replace the previous frame's alpha too when toggling back to transparency.
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);
    return FALSE;
  }), nullptr);
}

static void update_opaque_region(GtkWidget *window, gpointer) {
  if (auto *surface = gtk_widget_get_window(window)) gdk_window_set_opaque_region(surface, nullptr);
}

static gboolean draw_backing(GtkWidget *root, cairo_t *cr, gpointer data) {
  const auto *background = static_cast<TerminalBackground *>(data);
  auto *terminal = GTK_WIDGET(terminal_runtime_widget(background->window->runtime));
  gint x = 0, y = 0;
  if (!terminal || !gtk_widget_translate_coordinates(terminal, root, 0, 0, &x, &y)) return FALSE;
  GdkRGBA chrome{0.94, 0.94, 0.94, 1};
  gtk_style_context_lookup_color(gtk_widget_get_style_context(root), "theme_bg_color", &chrome);
  cairo_save(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgb(cr, chrome.red, chrome.green, chrome.blue);
  cairo_rectangle(cr, 0, 0, gtk_widget_get_allocated_width(root), gtk_widget_get_allocated_height(root));
  cairo_fill(cr);
  const auto &color = background->backing;
  cairo_set_source_rgba(cr, color.red / 255.0, color.green / 255.0, color.blue / 255.0,
      background->desktop_transparent ? 0 : 1);
  cairo_rectangle(cr, x, y, gtk_widget_get_allocated_width(terminal), gtk_widget_get_allocated_height(terminal));
  cairo_fill(cr);
  cairo_restore(cr);
  return FALSE;
}

static bool apply_native(TerminalBackground *background, const TerminalBackgroundSettings &settings,
    RgbColor backing) {
  auto *widget = terminal_runtime_widget(background->window->runtime);
  auto *view = boxrig_gtk3_terminal_widget_view(widget);
  const bool requested = settings.mode != TerminalBackgroundMode::opaque;
  auto config = boxrig_ui_transparency_config_default();
  config.request = requested ? BOXRIG_UI_TRANSPARENCY_PREFERRED : BOXRIG_UI_TRANSPARENCY_OPAQUE;
  config.requested_scopes = BOXRIG_UI_TRANSPARENCY_VIEW_SURFACE;
  config.background_opacity = requested ? static_cast<uint8_t>((settings.opacity * 255 + 50) / 100) : 255;
  boxrig_ui_transparency_state previous{};
  if (boxrig_gtk3_terminal_widget_get_transparency(widget, &previous) != BOXRIG_UI_OK ||
      boxrig_gtk3_terminal_widget_set_transparency(widget, &config) != BOXRIG_UI_OK) return false;
  const boxrig_ui_rgba tint{settings.tint.red, settings.tint.green, settings.tint.blue,
      static_cast<uint8_t>((settings.tint_opacity * 255 + 50) / 100)};
  if (boxrig_ui_view_set_background_color(view, tint) != BOXRIG_UI_OK) {
    boxrig_gtk3_terminal_widget_set_transparency(widget, &previous.config);
    return false;
  }
  background->settings = settings;
  background->backing = backing;
  boxrig_ui_transparency_state actual{};
  boxrig_gtk3_terminal_widget_get_transparency(widget, &actual);
  auto *window = background->window->window;
  background->desktop_transparent = requested && actual.effective == BOXRIG_UI_TRANSPARENCY_PREMULTIPLIED_ALPHA &&
      gdk_screen_is_composited(background->screen) &&
      gtk_widget_get_visual(window) == gdk_screen_get_rgba_visual(background->screen);
  update_opaque_region(window, nullptr);
  gtk_widget_queue_draw(window);
  terminal_runtime_refresh(background->window->runtime);
  return true;
}

static void clear_paths(TerminalBackground *background) {
  for (auto *widget : background->paths) {
    auto *context = gtk_widget_get_style_context(widget);
    gtk_style_context_remove_provider(context, GTK_STYLE_PROVIDER(background->provider));
    gtk_style_context_remove_class(context, "terminal-background-path");
    g_object_unref(widget);
  }
  background->paths.clear();
}

void attach_terminal_background(TerminalBackground *background) {
  clear_paths(background);
  auto *widget = GTK_WIDGET(terminal_runtime_widget(background->window->runtime));
  for (auto *parent = gtk_widget_get_parent(widget); parent; parent = gtk_widget_get_parent(parent)) {
    background->paths.push_back(GTK_WIDGET(g_object_ref(parent)));
    auto *context = gtk_widget_get_style_context(parent);
    gtk_style_context_add_class(context, "terminal-background-path");
    gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(background->provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 4);
    if (parent == background->window->window) break;
  }
  if (!apply_native(background, background->settings, background->backing))
    g_printerr("Failed to restore terminal background\n");
}

TerminalBackground *create_terminal_background(MainWindow *window) {
  auto *background = new TerminalBackground{window, gtk_css_provider_new(),
      GDK_SCREEN(g_object_ref(gtk_widget_get_screen(window->window))), {}};
  gtk_css_provider_load_from_data(background->provider,
      ".terminal-background-path { background-color: transparent; background-image: none; }", -1, nullptr);
  g_signal_connect(window->root_box, "draw", G_CALLBACK(draw_backing), background);
  g_signal_connect(window->window, "style-updated", G_CALLBACK(update_opaque_region), background);
  g_signal_connect(window->window, "realize", G_CALLBACK(update_opaque_region), background);
  g_signal_connect(background->screen, "composited-changed", G_CALLBACK(+[](GdkScreen *, gpointer data) {
    auto *background = static_cast<TerminalBackground *>(data);
    if (!apply_native(background, background->settings, background->backing))
      g_printerr("Failed to update terminal compositor state\n");
  }), background);
  attach_terminal_background(background);
  return background;
}

bool apply_terminal_background(TerminalBackground *background, const SettingsStore &settings) {
  return apply_native(background, terminal_background_settings(settings),
      general_color_settings(settings).background.value_or(RgbColor{0, 0, 0}));
}

std::string terminal_background_status(const TerminalBackground *background) {
  if (!background) return {};
  if (background->settings.mode == TerminalBackgroundMode::opaque) return _("Opaque background");
  return background->desktop_transparent ? _("Transparent background") :
      _("Opaque background: desktop transparency is unavailable");
}

void destroy_terminal_background(TerminalBackground *background) {
  if (!background) return;
  g_signal_handlers_disconnect_by_data(background->window->root_box, background);
  g_signal_handlers_disconnect_by_data(background->window->window, background);
  g_signal_handlers_disconnect_by_data(background->screen, background);
  clear_paths(background);
  g_object_unref(background->screen);
  g_object_unref(background->provider);
  delete background;
}
}
