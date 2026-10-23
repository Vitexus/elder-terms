// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/settings-widget.h>
#include <cardio.h>
#include <glib/gstdio.h>
#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>

static GtkWidget *find(GtkWidget *widget, const char *id) {
  if (std::strcmp(gtk_widget_get_name(widget), id) == 0) return widget;
  if (!GTK_IS_CONTAINER(widget)) return nullptr;
  auto *children = gtk_container_get_children(GTK_CONTAINER(widget));
  GtkWidget *result = nullptr;
  for (auto *item = children; item && !result; item = item->next)
    result = find(GTK_WIDGET(item->data), id);
  g_list_free(children);
  return result;
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

static GtkWidget *chooser() {
  auto *windows = gtk_window_list_toplevels();
  GtkWidget *result = nullptr;
  for (auto *item = windows; item; item = item->next)
    if (std::strcmp(gtk_widget_get_name(GTK_WIDGET(item->data)),
                    "settings_general_icon_dialog") == 0) result = GTK_WIDGET(item->data);
  g_list_free(windows);
  return result;
}

static std::string icon(const elder_terms::SettingsStore &store) {
  return elder_terms::setting_string_value_or_default(store,
      elder_terms::make_setting_key("general", "icon"), "default");
}

static bool shows_red(GtkWidget *image) {
  cairo_surface_t *surface = nullptr;
  g_object_get(image, "surface", &surface, nullptr);
  if (!surface) return false;
  auto *sample = gdk_pixbuf_get_from_surface(surface, 16, 16, 1, 1);
  cairo_surface_destroy(surface);
  if (!sample) return false;
  const auto *bytes = gdk_pixbuf_read_pixels(sample);
  const bool red = bytes[0] == 255 && bytes[1] == 0 && bytes[2] == 0;
  g_object_unref(sample);
  return red;
}

int main(int argc, char **argv) {
  using namespace elder_terms;
  gtk_init(&argc, &argv);
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  auto *directory = g_dir_make_tmp("elder-icon-editor-XXXXXX", nullptr);
  assert(directory);
  const auto path = std::string(directory) + "/test image.png";
  auto *pixels = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 24, 12);
  gdk_pixbuf_fill(pixels, 0xff0000ff);
  assert(gdk_pixbuf_save(pixels, path.c_str(), "png", nullptr, nullptr));
  g_object_unref(pixels);

  SettingsWidgetOptions options;
  options.store = create_default_settings(default_terminal_display_settings(12), "Icon test");
  std::string applied, saved;
  options.callbacks.apply = [&](const SettingsStore &store, SettingsWidgetAction action) {
    applied = icon(store);
    action.complete(true, {});
  };
  options.callbacks.save = [&](const SettingsStore &store, SettingsWidgetAction action) {
    saved = icon(store);
    action.complete(true, {});
  };
  auto *state = create_settings_widget(std::move(options));
  auto *root = settings_widget_root(state);
  auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_container_add(GTK_CONTAINER(window), root);
  g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
    destroy_settings_widget(static_cast<SettingsWidgetState *>(data));
  }), state);
  gtk_widget_show_all(window);
  auto *preview = find(root, "settings_general_icon_preview");
  auto *choose = find(root, "settings_general_icon_choose");
  auto *reset = find(root, "settings_general_icon_reset");
  auto *status = find(root, "settings_general_icon_status");
  assert(preview && choose && reset && status);
  assert(gtk_image_get_storage_type(GTK_IMAGE(preview)) == GTK_IMAGE_SURFACE);
  assert(!settings_widget_is_dirty(state));

  gtk_button_clicked(GTK_BUTTON(choose));
  GtkWidget *popup = nullptr;
  gtk_container_forall(GTK_CONTAINER(window), +[](GtkWidget *child, gpointer data) {
    if (GTK_IS_POPOVER(child)) *static_cast<GtkWidget **>(data) = child;
  }, &popup);
  assert(popup);
  gtk_button_clicked(GTK_BUTTON(find(popup, "settings_general_icon_file")));
  auto *dialog = chooser();
  assert(dialog && gtk_widget_get_visible(dialog) && !gtk_widget_get_sensitive(window));
  assert(gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), path.c_str()));
  wait_until([&] {
    auto *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    const bool selected = filename && path == filename;
    g_free(filename);
    return selected;
  });
  gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
  wait_until([&] { return icon(settings_widget_draft_store(state)) == "file:" + path; });
  wait_until([&] { return shows_red(preview) && !gtk_widget_get_visible(status); });
  assert(settings_widget_is_dirty(state) && settings_widget_is_valid(state));
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_apply_button")));
  assert(applied == "file:" + path);
  gtk_button_clicked(GTK_BUTTON(reset));
  assert(icon(settings_widget_draft_store(state)) == "default");
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_cancel_button")));
  assert(icon(settings_widget_draft_store(state)) == applied);
  gtk_button_clicked(GTK_BUTTON(reset));
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_save_button")));
  assert(saved == "default");

  auto missing = settings_widget_draft_store(state);
  set_explicit_setting_value(&missing, make_setting_key("general", "icon"),
                            std::string("file:") + path + ".missing");
  update_settings_widget_store(state, missing);
  wait_until([&] { return gtk_widget_get_visible(status) &&
      std::strlen(gtk_label_get_text(GTK_LABEL(status))) != 0; });
  assert(settings_widget_is_valid(state) && icon(settings_widget_draft_store(state)) == "file:" + path + ".missing");
  gtk_button_clicked(GTK_BUTTON(choose));
  gtk_button_clicked(GTK_BUTTON(find(popup, "settings_general_icon_file")));
  assert(chooser());
  gtk_widget_destroy(window);
  assert(!chooser());

  SettingsWidgetOptions global;
  global.store = missing;
  global.mode = SettingsWidgetMode::global_defaults;
  auto *defaults = create_settings_widget(std::move(global));
  assert(!find(settings_widget_root(defaults), "settings_general_icon_choose"));
  destroy_settings_widget(defaults);
  g_remove(path.c_str());
  g_rmdir(directory);
  g_free(directory);
  std::cout << "PASS connection icon selection, Apply, Save, Cancel and missing-file fallback\n";
}
