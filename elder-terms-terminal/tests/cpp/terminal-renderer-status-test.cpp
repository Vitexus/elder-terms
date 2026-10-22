// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/terminal-renderer-status.h"
#include <elder-terms/settings-widget.h>
#include <cassert>
#include <cstring>
#include <iostream>

static GtkWidget *find(GtkWidget *widget, const char *id) {
  if (std::strcmp(gtk_widget_get_name(widget), id) == 0) return widget;
  if (!GTK_IS_CONTAINER(widget)) return nullptr;
  auto *children = gtk_container_get_children(GTK_CONTAINER(widget));
  GtkWidget *result = nullptr;
  for (auto *item = children; item && !result; item = item->next) result = find(GTK_WIDGET(item->data), id);
  g_list_free(children);
  return result;
}

int main(int argc, char **argv) {
  using namespace elder_terms;
  gtk_init(&argc, &argv);
  boxrig_gtk_render_state render{};
  assert(format_terminal_renderer_status(render, "X11").text == "Initializing");
  render.active_backend = BOXRIG_GTK_RENDER_BACKEND_OPENGL;
  assert(format_terminal_renderer_status(render, "X11").text == "OpenGL / X11");
  render.is_gles = true;
  assert(format_terminal_renderer_status(render, "Wayland").text == "OpenGL ES / Wayland");
  render.pending = true;
  assert(format_terminal_renderer_status(render, "Wayland").text == "OpenGL ES / Wayland (switching)");
  render.pending = false;
  render.active_backend = BOXRIG_GTK_RENDER_BACKEND_CAIRO;
  render.requested_policy = BOXRIG_GTK_RENDER_OPENGL_PREFERRED;
  std::strcpy(render.message, "GL context unavailable");
  const auto fallback = format_terminal_renderer_status(render, "X11");
  assert(fallback.text == "Cairo / X11");
  assert(fallback.tooltip.find("GL context unavailable") != std::string::npos);
  assert(fallback.tooltip.find("OpenGL") != std::string::npos);
  for (const bool runtime : {false, true}) {
    SettingsWidgetOptions options;
    options.store = create_default_settings({}, "Renderer test");
    options.is_runtime = runtime;
    unsigned changed = 0;
    options.callbacks.changed = [&changed] { changed++; };
    auto *state = create_settings_widget(std::move(options));
    auto *root = settings_widget_root(state);
    auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
      destroy_settings_widget(static_cast<SettingsWidgetState *>(data));
    }), state);
    gtk_container_add(GTK_CONTAINER(window), root);
    const auto before = changed;
    settings_widget_set_runtime_status(state, fallback.text, fallback.tooltip);
    gtk_widget_show_all(window);
    while (g_main_context_iteration(nullptr, FALSE)) {}
    auto *label = find(root, "settings_runtime_status");
    assert((label != nullptr) == runtime);
    if (label) {
      assert(std::string(gtk_label_get_text(GTK_LABEL(label))) == fallback.text);
      assert(gtk_label_get_xalign(GTK_LABEL(label)) == 0);
      assert(gtk_label_get_ellipsize(GTK_LABEL(label)) == PANGO_ELLIPSIZE_END);
    }
    assert(!settings_widget_is_dirty(state) && changed == before);
    gtk_widget_destroy(window);
  }
  std::cout << "PASS renderer status, fallback, runtime-only footer and unchanged draft\n";
}
