#include <gtk/gtk.h>

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(window), "Appearance test backdrop");
  gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_DESKTOP);
  gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
  gtk_window_set_accept_focus(GTK_WINDOW(window), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(window), 1280, 720);
  g_signal_connect(window, "draw", G_CALLBACK(+[](GtkWidget *, cairo_t *cr, gpointer) {
    cairo_set_source_rgb(cr, 224.0 / 255, 48.0 / 255, 32.0 / 255);
    cairo_paint(cr);
    return TRUE;
  }), nullptr);
  gtk_widget_show_all(window);
  gtk_main();
}
