#include "terminal-renderer-status.h"
#include <gdk/gdkx.h>
#include <gdk/gdkwayland.h>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

TerminalRendererStatus format_terminal_renderer_status(const boxrig_gtk_render_state &state,
    const std::string &display) {
  std::string text = _("Initializing");
  if (state.active_backend != BOXRIG_GTK_RENDER_BACKEND_NONE) {
    text = state.active_backend == BOXRIG_GTK_RENDER_BACKEND_CAIRO ? "Cairo" :
        state.is_gles ? "OpenGL ES" : "OpenGL";
    text += " / " + display;
    if (state.pending) text += " (" + std::string(_("switching")) + ")";
  }
  std::string tooltip = text;
  if (state.active_backend == BOXRIG_GTK_RENDER_BACKEND_CAIRO &&
      state.requested_policy != BOXRIG_GTK_RENDER_CAIRO)
    tooltip += "\n" + std::string(_("Requested OpenGL; using Cairo"));
  if (state.message[0]) tooltip += "\n" + std::string(state.message);
  return {text, tooltip};
}

TerminalRendererStatus terminal_renderer_status(BoxrigGtk3TerminalWidget *widget) {
  boxrig_gtk_render_state state{};
  boxrig_gtk3_terminal_widget_get_render_state(widget, &state);
  auto *display = gtk_widget_get_display(GTK_WIDGET(widget));
  const char *name = GDK_IS_X11_DISPLAY(display) ? "X11" :
      GDK_IS_WAYLAND_DISPLAY(display) ? "Wayland" : _("Other display");
  return format_terminal_renderer_status(state, name);
}

}
