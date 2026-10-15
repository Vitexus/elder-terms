#include "terminal-runtime.h"

#include <gestament/gtk.h>
#include <glib-unix.h>
#include <csignal>
#include <cstdio>
#include <string>

struct Driver {
  elder_terms::TerminalRuntime *runtime = nullptr;
  GtkWidget *status = nullptr;
  std::string last_report;
};

static void report_state(void *context) {
  auto *driver = static_cast<Driver *>(context);
  auto *widget = elder_terms::terminal_runtime_widget(driver->runtime);
  boxrig_gtk_render_state render{};
  boxrig_font_effective_metrics metrics{};
  boxrig_ui_layout_result layout{};
  if (boxrig_gtk3_terminal_widget_get_render_state(widget, &render) != BOXRIG_UI_OK || render.pending ||
      boxrig_gtk3_terminal_widget_get_layout(widget, &layout) != BOXRIG_UI_OK ||
      boxrig_gtk3_terminal_widget_get_effective_metrics(widget, &metrics) != BOXRIG_FONT_CATALOG_OK) return;
  char text[256];
  const char *backend = render.active_backend == BOXRIG_GTK_RENDER_BACKEND_OPENGL ? "opengl" :
      render.active_backend == BOXRIG_GTK_RENDER_BACKEND_CAIRO ? "cairo" : "none";
  std::snprintf(text, sizeof(text), "backend=%s grid=%zux%zu font=%.2f reason=%d",
                backend, layout.columns, layout.rows, metrics.requested_size_dip, render.reason);
  // AT-SPI identifies this widget as a terminal. Also publish its actual
  // AtkText snapshot for automation clients without a terminal text adapter.
  auto *accessible = gtk_widget_get_accessible(GTK_WIDGET(widget));
  auto *visible_text = atk_text_get_text(ATK_TEXT(accessible), 0, -1);
  auto *encoded = g_base64_encode(reinterpret_cast<const guchar *>(visible_text),
      std::char_traits<char>::length(visible_text));
  const std::string report = std::string(text) + " cell=" + std::to_string(metrics.cell_width) +
      "x" + std::to_string(metrics.cell_height) + " selected=" + std::to_string(atk_text_get_n_selections(ATK_TEXT(accessible))) +
      " ready=" + (elder_terms::terminal_runtime_is_settled(driver->runtime) ? "1" : "0") +
      " text=" + encoded;
  g_free(encoded);
  g_free(visible_text);
  if (report != driver->last_report) {
    driver->last_report = report;
    if (std::string_view(gtk_label_get_text(GTK_LABEL(driver->status))) != text)
      gtk_label_set_text(GTK_LABEL(driver->status), text);
    std::printf("%s\n", report.c_str());
    std::fflush(stdout);
  }
}

static void zoom_in(GtkButton *, gpointer context) {
  auto *driver = static_cast<Driver *>(context);
  elder_terms::terminal_runtime_visual_zoom(driver->runtime, BOXRIG_FONT_ZOOM_INCREASE);
}

static void copy_selection(GtkButton *, gpointer context) {
  auto *driver = static_cast<Driver *>(context);
  const boxrig_ui_view_action action{BOXRIG_UI_VIEW_ACTION_COPY, BOXRIG_UI_CLIPBOARD_TARGET_CLIPBOARD};
  const auto status = boxrig_gtk3_terminal_widget_apply_view_action(elder_terms::terminal_runtime_widget(driver->runtime), &action);
  if (status != BOXRIG_UI_OK && status != BOXRIG_UI_UNAVAILABLE)
    g_warning("boxrig copy failed: %d", status);
}

static gboolean close_window(GtkWidget *, GdkEvent *, gpointer) {
  gtk_main_quit();
  return TRUE;
}

static gboolean stop_driver(gpointer) { gtk_main_quit(); return G_SOURCE_CONTINUE; }

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  Driver driver;
  auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(window), "boxrig driver");
  gestament_gtk_assign_accessible_id(window, "driver_window");
  auto *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(window), content);
  auto *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_pack_start(GTK_BOX(content), controls, FALSE, FALSE, 0);
  auto *zoom = gtk_button_new_with_label("Zoom in");
  gestament_gtk_assign_accessible_id(zoom, "driver_zoom_in");
  gtk_box_pack_start(GTK_BOX(controls), zoom, FALSE, FALSE, 0);
  auto *copy = gtk_button_new_with_label("Copy selection");
  gestament_gtk_assign_accessible_id(copy, "driver_copy");
  gtk_box_pack_start(GTK_BOX(controls), copy, FALSE, FALSE, 0);
  driver.status = gtk_label_new("Starting boxrig");
  gestament_gtk_assign_accessible_id(driver.status, "driver_status");
  gtk_box_pack_start(GTK_BOX(controls), driver.status, FALSE, FALSE, 0);
  driver.runtime = elder_terms::create_terminal_runtime(GTK_WINDOW(window), GTK_BOX(content));
  if (!driver.runtime) { gtk_widget_destroy(window); return 1; }
  auto *widget = elder_terms::terminal_runtime_widget(driver.runtime);
  gestament_gtk_assign_accessible_id(GTK_WIDGET(widget), "driver_terminal");
  elder_terms::terminal_runtime_set_changed_callback(driver.runtime, report_state, &driver);
  g_signal_connect(zoom, "clicked", G_CALLBACK(zoom_in), &driver);
  g_signal_connect(copy, "clicked", G_CALLBACK(copy_selection), &driver);
  g_signal_connect(window, "delete-event", G_CALLBACK(close_window), nullptr);
  const auto signal_source = g_unix_signal_add(SIGTERM, stop_driver, nullptr);
  std::string output;
  for (unsigned int line = 1; line <= 120; ++line) output += "History line " + std::to_string(line) + "\r\n";
  output += "\033[2J\033[H日本語の端末 — boxrig\r\n\033[1;32mBold green text\033[0m\r\nSelect and copy this text.\r\n";
  const bool fed = elder_terms::terminal_runtime_feed(driver.runtime, output);
  gtk_widget_show_all(window);
  gtk_widget_grab_focus(GTK_WIDGET(widget));
  if (fed) gtk_main();
  g_source_remove(signal_source);
  elder_terms::destroy_terminal_runtime(driver.runtime);
  gtk_widget_destroy(window);
  return fed ? 0 : 1;
}
