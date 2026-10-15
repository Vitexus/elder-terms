#include "../../src/terminal-runtime.h"

#include <cstdio>
#include <string_view>

using namespace elder_terms;

struct Fixture {
  TerminalRuntime *runtime = nullptr;
  guint observation = 0;
  bool fed = false;
  bool notified_ready = false;
  bool failed = false;
  boxrig_gtk_render_backend backend = BOXRIG_GTK_RENDER_BACKEND_OPENGL;
  std::uint64_t previous_generation = 0;
};

static gboolean observe(gpointer context) {
  auto *fixture = static_cast<Fixture *>(context);
  fixture->observation = 0;
  auto *widget = terminal_runtime_widget(fixture->runtime);
  auto *view = boxrig_gtk3_terminal_widget_view(widget);
  boxrig_ui_size_command_state size{};
  if (!boxrig_gtk3_terminal_widget_has_presentation(widget) ||
      boxrig_ui_view_frame_requested(view) ||
      boxrig_gtk3_terminal_widget_get_size_command_state(widget, &size) != BOXRIG_UI_OK ||
      size != BOXRIG_UI_SIZE_COMMAND_IDLE) return G_SOURCE_REMOVE;
  const auto generation = boxrig_gtk3_terminal_widget_presentation_generation(widget);
  if (!fixture->fed) {
    fixture->fed = true;
    fixture->previous_generation = generation;
    fixture->notified_ready = false;
    terminal_runtime_feed(fixture->runtime, "Updated native presentation");
    return G_SOURCE_REMOVE;
  }
  if (generation > fixture->previous_generation) {
    boxrig_gtk_render_state render{};
    if (boxrig_gtk3_terminal_widget_get_render_state(widget, &render) != BOXRIG_UI_OK || render.active_backend != fixture->backend) {
      std::fputs("Requested presentation backend is unavailable\n", stderr);
      fixture->failed = true;
    }
    // Native drawing has finished and higher-priority layout dispatches have
    // drained. Readiness must agree with the actual current presentation.
    if (!terminal_runtime_is_settled(fixture->runtime) || !fixture->notified_ready) {
      std::fputs("Native presentation completed without a settled runtime notification\n", stderr);
      fixture->failed = true;
    }
    gtk_main_quit();
  }
  return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  auto *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(window), content);
  Fixture fixture;
  if (argc > 1 && std::string_view(argv[1]) == "cairo") fixture.backend = BOXRIG_GTK_RENDER_BACKEND_CAIRO;
  fixture.runtime = create_terminal_runtime(GTK_WINDOW(window), GTK_BOX(content), GTK_BOX(content));
  if (!fixture.runtime) return 1;
  terminal_runtime_set_changed_callback(fixture.runtime, [](void *context) {
    auto *fixture = static_cast<Fixture *>(context);
    fixture->notified_ready = terminal_runtime_is_settled(fixture->runtime);
  }, &fixture);
  g_signal_connect_after(terminal_runtime_widget(fixture.runtime), "draw",
      G_CALLBACK(+[](GtkWidget *, cairo_t *, gpointer context) -> gboolean {
        auto *fixture = static_cast<Fixture *>(context);
        if (!fixture->observation) fixture->observation = g_idle_add(observe, fixture);
        return FALSE;
      }), &fixture);
  gtk_widget_show_all(window);
  gtk_main();
  if (fixture.observation) g_source_remove(fixture.observation);
  destroy_terminal_runtime(fixture.runtime);
  gtk_widget_destroy(window);
  return fixture.failed ? 1 : 0;
}
