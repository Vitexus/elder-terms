// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <gtk/gtk.h>
#include <cstring>

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  const bool checker = argc > 1 && std::strcmp(argv[1], "checker") == 0;
  auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(window), "Appearance test backdrop");
  gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_DESKTOP);
  gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
  gtk_window_set_accept_focus(GTK_WINDOW(window), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(window), 1280, 720);
  g_signal_connect(window, "draw", G_CALLBACK(+[](GtkWidget *widget, cairo_t *cr, gpointer data) {
    if (GPOINTER_TO_INT(data)) {
      for (int y = 0; y < gtk_widget_get_allocated_height(widget); y += 8) {
        for (int x = 0; x < gtk_widget_get_allocated_width(widget); x += 8) {
          const double value = (x / 8 + y / 8) % 2;
          cairo_set_source_rgb(cr, value, value, value);
          cairo_rectangle(cr, x, y, 8, 8);
          cairo_fill(cr);
        }
      }
    } else {
      cairo_set_source_rgb(cr, 224.0 / 255, 48.0 / 255, 32.0 / 255);
      cairo_paint(cr);
    }
    return TRUE;
  }), GINT_TO_POINTER(checker));
  gtk_widget_show_all(window);
  gtk_main();
}
