#include "terminal-runtime.h"

#include <cstdint>
#include <limits>
#include <new>
#include <string>

namespace elder_terms {

struct TerminalRuntime {
  boxrig_terminal *terminal = nullptr;
  boxrig_ui_model *model = nullptr;
  BoxrigGtk3TerminalWidget *widget = nullptr;
  GtkWidget *scroll = nullptr;
  GtkWindow *window = nullptr;
  GtkBox *content = nullptr;
  guint frame_source = 0;
  bool stopped = false;
  bool frame_pending = true;
  bool selection_drag_active = false;
  std::uint64_t allocation_generation = 0;
  std::uint64_t command_allocation = 0;
  boxrig_ui_layout_action size_command{};
  int command_start_width = 0;
  int command_start_height = 0;
  boxrig_ui_window_state window_state = BOXRIG_UI_WINDOW_NORMAL;
  void (*changed)(void *) = nullptr;
  void *changed_context = nullptr;
};

static void request_frame(void *context);

static boxrig_input_mouse_route route_mouse(
    const boxrig_mouse_event *event, const boxrig_ui_hit_test_result *hit,
    void *context) {
  auto *runtime = static_cast<TerminalRuntime *>(context);
  auto *view = boxrig_gtk3_terminal_widget_view(runtime->widget);
  const bool release = event->action == BOXRIG_MOUSE_RELEASE &&
                       event->button == BOXRIG_MOUSE_BUTTON_LEFT;
  if (runtime->selection_drag_active) {
    if ((event->action == BOXRIG_MOUSE_MOTION || release) &&
        hit->inside_grid && hit->text_position_valid) {
      const auto status = boxrig_ui_view_update_selection(view, hit);
      if (status == BOXRIG_UI_STALE) runtime->selection_drag_active = false;
      else if (status != BOXRIG_UI_OK) return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
    if (release) {
      runtime->selection_drag_active = false;
      const boxrig_ui_view_action copy{BOXRIG_UI_VIEW_ACTION_COPY,
          BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION};
      const auto status = boxrig_gtk3_terminal_widget_apply_view_action(runtime->widget, &copy);
      if (status != BOXRIG_UI_OK && status != BOXRIG_UI_UNAVAILABLE)
        return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
    request_frame(runtime);
    return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
  }
  if (event->action != BOXRIG_MOUSE_PRESS || !hit->inside_grid)
    return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
  const bool override = (event->modifiers & (BOXRIG_MODIFIER_SHIFT | BOXRIG_MODIFIER_CONTROL)) != 0;
  if (boxrig_terminal_mouse_reporting_enabled(runtime->terminal) && !override)
    return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
  if (event->button == BOXRIG_MOUSE_BUTTON_LEFT && hit->text_position_valid) {
    boxrig_ui_selection_options options{};
    options.shape = (event->modifiers & BOXRIG_MODIFIER_CONTROL) ?
        BOXRIG_UI_SELECTION_RECTANGULAR : BOXRIG_UI_SELECTION_LINEAR;
    options.kind = options.shape == BOXRIG_UI_SELECTION_RECTANGULAR ? BOXRIG_UI_SELECTION_CHARACTER :
        event->click_count >= 3 ? BOXRIG_UI_SELECTION_LINE :
        event->click_count == 2 ? BOXRIG_UI_SELECTION_WORD : BOXRIG_UI_SELECTION_CHARACTER;
    const auto status = boxrig_ui_view_begin_selection_with_options(view, &options, hit);
    if (status == BOXRIG_UI_STALE) return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
    if (status != BOXRIG_UI_OK) return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    runtime->selection_drag_active = true;
    request_frame(runtime);
    return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
  }
  if (event->button == BOXRIG_MOUSE_BUTTON_WHEEL_UP || event->button == BOXRIG_MOUSE_BUTTON_WHEEL_DOWN) {
    if ((event->modifiers & BOXRIG_MODIFIER_CONTROL) != 0 ||
        (!override && boxrig_terminal_alternate_scroll_enabled(runtime->terminal) &&
         boxrig_terminal_active_text_screen(runtime->terminal) == BOXRIG_TEXT_SCREEN_ALTERNATE))
      return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
    boxrig_scrollbar_model model{};
    if (boxrig_scrollbar_model_read(runtime->terminal, &model) != 0)
      return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    if (model.enabled) {
      const auto position = event->button == BOXRIG_MOUSE_BUTTON_WHEEL_UP ?
          (model.position > 3 ? model.position - 3 : 0) :
          (model.history_rows - model.position > 3 ? model.position + 3 : model.history_rows);
      bool changed = false;
      if (boxrig_scrollbar_apply_action(runtime->terminal, BOXRIG_SCROLLBAR_POSITION, position, &changed) != 0)
        return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
      if (changed) request_frame(runtime);
    }
    return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
  }
  return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
}

static bool ui_result(boxrig_ui_status status, const char *operation) {
  if (status == BOXRIG_UI_OK) return true;
  g_warning("boxrig %s failed: %d", operation, status);
  return false;
}

static boxrig_ui_fixed allocation_fixed(TerminalRuntime *runtime, bool width) {
  auto *widget = GTK_WIDGET(runtime->widget);
  const auto extent = width ? gtk_widget_get_allocated_width(widget)
                            : gtk_widget_get_allocated_height(widget);
  return static_cast<boxrig_ui_fixed>(extent) *
         gtk_widget_get_scale_factor(widget) * BOXRIG_UI_FIXED_ONE;
}

static bool complete_size(TerminalRuntime *runtime, boxrig_ui_size_outcome outcome) {
  bool applied = false;
  boxrig_ui_layout_result layout{};
  const auto revision = runtime->size_command.revision;
  runtime->size_command = {};
  return ui_result(boxrig_gtk3_terminal_widget_complete_size_command(
      runtime->widget, revision, outcome, allocation_fixed(runtime, true),
      allocation_fixed(runtime, false), &applied, &layout), "complete size");
}

static bool process_layout(TerminalRuntime *runtime) {
  auto *widget = GTK_WIDGET(runtime->widget);
  if (runtime->size_command.revision != 0) {
    boxrig_ui_size_command_state state{};
    if (!ui_result(boxrig_gtk3_terminal_widget_get_size_command_state(
        runtime->widget, &state), "size state")) return false;
    if (state != BOXRIG_UI_SIZE_COMMAND_DISPATCHED) {
      runtime->size_command = {};
    } else {
      if (runtime->allocation_generation == runtime->command_allocation) return true;
      const auto &requested = runtime->size_command.value.content_size;
      const bool exact = allocation_fixed(runtime, true) == requested.width &&
                         allocation_fixed(runtime, false) == requested.height;
      const bool changed = gtk_widget_get_allocated_width(widget) != runtime->command_start_width ||
                           gtk_widget_get_allocated_height(widget) != runtime->command_start_height;
      const auto outcome = exact ? BOXRIG_UI_SIZE_ACCEPTED :
          runtime->window_state != BOXRIG_UI_WINDOW_NORMAL ? BOXRIG_UI_SIZE_CONSTRAINED :
          changed ? BOXRIG_UI_SIZE_SUPERSEDED : BOXRIG_UI_SIZE_REJECTED;
      return complete_size(runtime, outcome);
    }
  }
  boxrig_ui_layout_action action{};
  if (!boxrig_gtk3_terminal_widget_take_layout_action(runtime->widget, &action)) return true;
  if (action.kind == BOXRIG_UI_LAYOUT_ACTION_GRID_RESIZE) {
    const auto &grid = action.value.grid_resize;
    const bool resized = boxrig_terminal_resize(runtime->terminal, grid.rows, grid.columns) == BOXRIG_STATUS_OK;
    return ui_result(boxrig_gtk3_terminal_widget_complete_grid_resize(
        runtime->widget, action.revision, resized), "complete grid") && resized;
  }
  runtime->size_command = action;
  if (runtime->window_state != BOXRIG_UI_WINDOW_NORMAL) {
    return complete_size(runtime, BOXRIG_UI_SIZE_CONSTRAINED);
  }
  const auto divisor = static_cast<boxrig_ui_fixed>(gtk_widget_get_scale_factor(widget)) * BOXRIG_UI_FIXED_ONE;
  const auto logical_width = (action.value.content_size.width + divisor - 1) / divisor;
  const auto logical_height = (action.value.content_size.height + divisor - 1) / divisor;
  const auto extra_width = gtk_widget_get_allocated_width(GTK_WIDGET(runtime->content)) - gtk_widget_get_allocated_width(widget);
  const auto extra_height = gtk_widget_get_allocated_height(GTK_WIDGET(runtime->content)) - gtk_widget_get_allocated_height(widget);
  if (logical_width <= 0 || logical_height <= 0 || extra_width < 0 || extra_height < 0 ||
      logical_width > std::numeric_limits<int>::max() - extra_width ||
      logical_height > std::numeric_limits<int>::max() - extra_height) {
    return complete_size(runtime, BOXRIG_UI_SIZE_REJECTED);
  }
  runtime->command_allocation = runtime->allocation_generation;
  runtime->command_start_width = gtk_widget_get_allocated_width(widget);
  runtime->command_start_height = gtk_widget_get_allocated_height(widget);
  if (!ui_result(boxrig_gtk3_terminal_widget_set_geometry_hints(
      runtime->widget, runtime->window, true), "geometry hints")) return false;
  // GtkWindow accounts for CSD itself; only actual content siblings are added.
  gtk_window_resize(runtime->window, static_cast<int>(logical_width) + extra_width,
                    static_cast<int>(logical_height) + extra_height);
  return true;
}

static bool process_host_items(TerminalRuntime *runtime) {
  boxrig_host_item item{};
  for (unsigned int work = 0; work < 64 && boxrig_terminal_host_item_peek(runtime->terminal, &item); ++work) {
    if (item.type == BOXRIG_HOST_ITEM_TITLE) {
      const std::string title(item.payload_length == 0 ? "" :
          reinterpret_cast<const char *>(item.payload), item.payload_length);
      if (g_utf8_validate(title.c_str(), title.size(), nullptr)) gtk_window_set_title(runtime->window, title.c_str());
    }
    const auto request_id = item.request_id;
    if (boxrig_terminal_host_item_consume(runtime->terminal) != BOXRIG_STATUS_OK) return false;
    if (request_id != 0 && boxrig_terminal_complete_host_request(runtime->terminal,
        request_id, BOXRIG_HOST_REQUEST_UNAVAILABLE, nullptr, 0) != BOXRIG_STATUS_OK) return false;
  }
  if (boxrig_terminal_host_item_peek(runtime->terminal, &item)) request_frame(runtime);
  return true;
}

static gboolean dispatch_frame(gpointer context) {
  auto *runtime = static_cast<TerminalRuntime *>(context);
  runtime->frame_source = 0;
  if (runtime->stopped || !gtk_widget_get_mapped(GTK_WIDGET(runtime->widget))) return G_SOURCE_REMOVE;
  boxrig_ui_component_frame_result result{};
  if (!process_host_items(runtime) || !process_layout(runtime) ||
      !ui_result(boxrig_ui_model_synchronize(runtime->model), "synchronize") ||
      !ui_result(boxrig_gtk3_terminal_widget_frame(runtime->widget, &result), "frame")) {
    runtime->stopped = true;
    boxrig_gtk3_terminal_widget_quiesce(runtime->widget);
    return G_SOURCE_REMOVE;
  }
  // A pending presentation may be waiting for GTK allocation or paint. Only
  // additional_dispatch requests another immediate pass through the main loop.
  if (result.additional_dispatch) request_frame(runtime);
  runtime->frame_pending = result.frame_pending;
  if (runtime->changed) runtime->changed(runtime->changed_context);
  return G_SOURCE_REMOVE;
}

static void request_frame(void *context) {
  auto *runtime = static_cast<TerminalRuntime *>(context);
  if (!runtime->stopped && runtime->frame_source == 0) {
    // Dispatch accepted font/layout requests before GTK's resize priority.
    // The callback still only schedules work; no frame runs recursively.
    runtime->frame_source = g_idle_add_full(G_PRIORITY_DEFAULT, dispatch_frame, runtime, nullptr);
  }
}

static void allocated(GtkWidget *, GtkAllocation *, gpointer context) {
  auto *runtime = static_cast<TerminalRuntime *>(context);
  ++runtime->allocation_generation;
  request_frame(runtime);
}

static void mapped(GtkWidget *, gpointer context) { request_frame(context); }

static gboolean window_state_changed(GtkWidget *, GdkEventWindowState *event, gpointer context) {
  auto *runtime = static_cast<TerminalRuntime *>(context);
  const auto flags = event->new_window_state;
  runtime->window_state = (flags & GDK_WINDOW_STATE_FULLSCREEN) ? BOXRIG_UI_WINDOW_FULLSCREEN :
      (flags & GDK_WINDOW_STATE_MAXIMIZED) ? BOXRIG_UI_WINDOW_MAXIMIZED :
      (flags & GDK_WINDOW_STATE_TILED) ? BOXRIG_UI_WINDOW_TILED : BOXRIG_UI_WINDOW_NORMAL;
  ui_result(boxrig_gtk3_terminal_widget_set_window_state(runtime->widget, runtime->window_state), "window state");
  request_frame(runtime);
  return FALSE;
}

TerminalRuntime *create_terminal_runtime(GtkWindow *window, GtkBox *content) {
  auto *runtime = new (std::nothrow) TerminalRuntime;
  if (!runtime) return nullptr;
  runtime->window = window;
  runtime->content = content;
  auto config = boxrig_terminal_config_default();
  config.rows = 24;
  config.columns = 80;
  auto widget_config = boxrig_gtk3_terminal_widget_config_default();
  widget_config.render_policy = BOXRIG_GTK_RENDER_OPENGL_PREFERRED;
  widget_config.ime_policy = BOXRIG_GTK_IME_AUTO;
  widget_config.frame_request_callback = request_frame;
  widget_config.frame_request_context = runtime;
  if (boxrig_terminal_create(&config, &runtime->terminal) != BOXRIG_STATUS_OK ||
      !ui_result(boxrig_ui_model_create(runtime->terminal, &runtime->model), "create model") ||
      !ui_result(boxrig_gtk3_terminal_widget_create(runtime->model, &widget_config, &runtime->widget), "create widget")) {
    destroy_terminal_runtime(runtime);
    return nullptr;
  }
  g_object_ref_sink(runtime->widget);
  runtime->scroll = GTK_WIDGET(boxrig_gtk3_terminal_widget_create_scroll_container(runtime->widget));
  if (!runtime->scroll) { destroy_terminal_runtime(runtime); return nullptr; }
  g_object_ref_sink(runtime->scroll);
  gtk_box_pack_start(content, runtime->scroll, TRUE, TRUE, 0);
  g_signal_connect(window, "size-allocate", G_CALLBACK(allocated), runtime);
  g_signal_connect(window, "window-state-event", G_CALLBACK(window_state_changed), runtime);
  g_signal_connect(runtime->widget, "map", G_CALLBACK(mapped), runtime);
  if (!ui_result(boxrig_gtk3_terminal_widget_set_preferred_grid(runtime->widget, config.rows, config.columns), "preferred grid") ||
      !ui_result(boxrig_gtk3_terminal_widget_set_mouse_router(runtime->widget, route_mouse, runtime), "mouse router") ||
      !ui_result(boxrig_gtk3_terminal_widget_set_visible(runtime->widget, true), "visibility")) {
    destroy_terminal_runtime(runtime);
    return nullptr;
  }
  return runtime;
}

void destroy_terminal_runtime(TerminalRuntime *runtime) {
  if (!runtime) return;
  runtime->stopped = true;
  if (runtime->frame_source) g_source_remove(runtime->frame_source);
  g_signal_handlers_disconnect_by_data(runtime->window, runtime);
  if (runtime->widget) {
    g_signal_handlers_disconnect_by_data(runtime->widget, runtime);
    boxrig_gtk3_terminal_widget_quiesce(runtime->widget);
    gtk_widget_destroy(GTK_WIDGET(runtime->widget));
    g_object_unref(runtime->widget);
  }
  if (runtime->scroll) {
    gtk_widget_destroy(runtime->scroll);
    g_object_unref(runtime->scroll);
  }
  boxrig_ui_model_destroy(runtime->model);
  boxrig_terminal_destroy(runtime->terminal);
  delete runtime;
}

BoxrigGtk3TerminalWidget *terminal_runtime_widget(TerminalRuntime *runtime) { return runtime->widget; }

bool terminal_runtime_feed(TerminalRuntime *runtime, std::string_view text) {
  size_t consumed = 0;
  const auto status = boxrig_terminal_feed(runtime->terminal,
      reinterpret_cast<const uint8_t *>(text.data()), text.size(), &consumed);
  request_frame(runtime);
  return status == BOXRIG_STATUS_OK && consumed == text.size();
}

void terminal_runtime_set_changed_callback(TerminalRuntime *runtime, void (*callback)(void *), void *context) {
  runtime->changed = callback;
  runtime->changed_context = context;
}

boxrig_ui_status terminal_runtime_visual_zoom(TerminalRuntime *runtime, boxrig_font_zoom_direction direction) {
  bool changed = false;
  const auto status = boxrig_gtk3_terminal_widget_visual_zoom(runtime->widget,
      direction, boxrig_font_visual_step_policy_default(), &changed);
  if (status == BOXRIG_UI_OK && changed) request_frame(runtime);
  return status;
}

bool terminal_runtime_is_settled(const TerminalRuntime *runtime) {
  boxrig_ui_size_command_state state{};
  return !runtime->stopped && !runtime->frame_pending && runtime->frame_source == 0 &&
      runtime->size_command.revision == 0 &&
      boxrig_gtk3_terminal_widget_get_size_command_state(runtime->widget, &state) == BOXRIG_UI_OK &&
      state == BOXRIG_UI_SIZE_COMMAND_IDLE;
}

}
