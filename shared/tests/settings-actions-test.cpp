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
  SettingsWidgetOptions options;
  options.store = create_default_settings(default_terminal_display_settings(12), "Action test");
  options.is_runtime = true;
  SettingsWidgetAction pending;
  bool asynchronous = false;
  options.callbacks.apply = [&](const SettingsStore &, SettingsWidgetAction action) {
    if (asynchronous) pending = std::move(action);
    else action.complete(false, "Rejected for testing");
  };
  options.callbacks.save = [](const SettingsStore &, SettingsWidgetAction action) {
    action.complete(false, "Save failed for testing");
  };
  auto *state = create_settings_widget(std::move(options));
  auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  auto *root = settings_widget_root(state);
  gtk_container_add(GTK_CONTAINER(window), root);
  g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
    destroy_settings_widget(static_cast<SettingsWidgetState *>(data));
  }), state);
  auto *opacity = find(root, "settings_terminal_background_opacity_entry");
  gtk_entry_set_text(GTK_ENTRY(opacity), "101");
  assert(!gtk_widget_get_sensitive(find(root, "settings_apply_button")));
  gtk_entry_set_text(GTK_ENTRY(opacity), "0");
  assert(gtk_widget_get_sensitive(find(root, "settings_apply_button")));
  auto *tint = find(root, "settings_terminal_background_tint_color_entry");
  gtk_entry_set_text(GTK_ENTRY(tint), "blue");
  assert(!gtk_widget_get_sensitive(find(root, "settings_save_button")));
  gtk_entry_set_text(GTK_ENTRY(tint), "#0000ff");
  assert(gtk_widget_get_sensitive(find(root, "settings_save_button")));
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_cancel_button")));
  assert(terminal_background_settings(settings_widget_draft_store(state)).opacity == 80);
  auto *shape = find(root, "settings_terminal_cursor_shape_combo");
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(shape), "bar");
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_apply_button")));
  assert(settings_widget_is_dirty(state));
  assert(std::string(gtk_label_get_text(GTK_LABEL(find(root, "settings_action_message")))) == "Rejected for testing");
  assert(gtk_widget_get_sensitive(find(root, "settings_apply_button")));
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_cancel_button")));
  assert(terminal_cursor_settings(settings_widget_draft_store(state)).shape == TerminalCursorShape::block);
  asynchronous = true;
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(shape), "bar");
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_apply_button")));
  assert(pending.current());
  assert(!gtk_widget_get_sensitive(find(root, "settings_apply_button")));
  assert(!gtk_widget_get_sensitive(find(root, "settings_save_button")));
  assert(gtk_widget_get_sensitive(find(root, "settings_cancel_button")));
  // Editing while preparation is pending invalidates the old snapshot.
  const auto stale = pending;
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(shape), "underline");
  assert(!stale.current());
  stale.complete(true, {});
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_apply_button")));
  assert(pending.current());
  pending.complete(true, {});
  assert(!pending.current());
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(shape), "bar");
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_save_button")));
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_cancel_button")));
  assert(terminal_cursor_settings(settings_widget_draft_store(state)).shape == TerminalCursorShape::underline);
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(shape), "bar");
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_apply_button")));
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_cancel_button")));
  assert(!pending.current());
  pending.complete(true, {});
  assert(terminal_cursor_settings(settings_widget_draft_store(state)).shape == TerminalCursorShape::underline);
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(shape), "bar");
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_apply_button")));
  gtk_widget_destroy(window);
  assert(!pending.current());
  pending.complete(true, {});
  std::cout << "PASS failed, pending, accepted, edited, cancelled and destroyed settings actions\n";
}
