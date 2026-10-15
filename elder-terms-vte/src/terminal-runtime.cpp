#include "terminal-runtime.h"

#include <cstdint>
#include <limits>
#include <new>
#include <array>
#include <optional>
#include <string>
#include <variant>
#include <type_traits>

namespace elder_terms {

using TerminalServiceRequest = std::variant<TerminalTextSendRequest, TerminalTransferRequest>;

struct TerminalRuntime {
  boxrig_terminal *terminal = nullptr;
  boxrig_ui_model *model = nullptr;
  TerminalConnection *connection = nullptr;
  std::function<void(std::span<const unsigned char>, std::span<const unsigned char>)> output;
  std::optional<std::variant<TerminalLocalCommand, TerminalTransportRequest>> pending_connection;
  std::optional<TerminalTextSettings> pending_input;
  std::optional<std::string> pending_encoding;
  std::optional<cardio::promise<void>> encoding_completion;
  bool encoding_active = false;
  std::optional<TerminalServiceRequest> pending_service;
  std::optional<cardio::promise<void>> service_completion;
  cardio::cancellation_source service_stop;
  bool service_active = false;
  TerminalZmodemAutoStart auto_start;
  bool auto_selecting = false;
  std::optional<std::pair<TerminalTransferDirection, TerminalZmodemAutoStart>> pending_auto_start;
  std::optional<cardio::promise<void>> break_completion;
  bool break_active = false;
  TerminalTextSettings applied_input;
  bool input_allowed = true;
  BoxrigGtk3TerminalWidget *widget = nullptr;
  GtkWidget *scroll = nullptr;
  GtkWindow *window = nullptr;
  GtkBox *content = nullptr;
  guint frame_source = 0;
  bool stopped = false;
  bool ui_failed = false;
  bool widget_allocated = false;
  bool frame_pending = true;
  bool selection_drag_active = false;
  std::uint64_t allocation_generation = 0;
  std::uint64_t command_allocation = 0;
  boxrig_ui_layout_action size_command{};
  bool grid_pending = false;
  std::optional<cardio::promise<void>> grid_completion;
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

static void fail_ui(TerminalRuntime *runtime) {
  runtime->ui_failed = true;
  runtime->pending_connection.reset();
  runtime->pending_encoding.reset();
  terminal_runtime_cancel_transfer(runtime);
  terminal_connection_stop(runtime->connection);
  boxrig_gtk3_terminal_widget_quiesce(runtime->widget);
  request_frame(runtime);
}

static cardio::promise<void> change_encoding(TerminalRuntime *runtime, std::string encoding) {
  co_await terminal_connection_change_encoding(runtime->connection, std::move(encoding));
  runtime->encoding_active = false;
  request_frame(runtime);
}

static void finish_service(TerminalRuntime *runtime, const TerminalServiceRequest &request, bool succeeded) {
  runtime->service_active = false;
  request_frame(runtime);
  std::visit([succeeded](const auto &value) {
    if (value.active) value.active(false);
    if (value.finished) value.finished(succeeded);
  }, request);
}

static cardio::promise<void> run_service(TerminalRuntime *runtime, TerminalServiceRequest request) {
  const auto succeeded = co_await std::visit([runtime](auto value) {
    if constexpr (std::is_same_v<decltype(value), TerminalTextSendRequest>)
      return terminal_connection_send_text(runtime->connection, std::move(value), runtime->service_stop.get_cancellation());
    else return terminal_connection_transfer(runtime->connection, std::move(value), runtime->service_stop.get_cancellation());
  }, request);
  finish_service(runtime, request, succeeded);
}

static cardio::promise<void> run_auto_start(TerminalRuntime *runtime,
    std::pair<TerminalTransferDirection, TerminalZmodemAutoStart> selection) {
  std::optional<TerminalTransferRequest> selected;
  const auto cancellation = runtime->service_stop.get_cancellation();
  try {
    cancellation.throw_if_cancellation_requested();
    selected = co_await selection.second(selection.first, cancellation);
  } catch (const cardio::canceled_exception &) {
  } catch (const std::exception &error) {
    g_printerr("ZMODEM selection: %s\n", error.what());
  }
  runtime->auto_selecting = false;
  auto request = selected.value_or(TerminalTransferRequest{});
  request.protocol = TerminalTransferProtocol::zmodem;
  request.direction = selection.first;
  if (!selected || (request.direction == TerminalTransferDirection::send && request.source_file_uris.empty()))
    runtime->service_stop.cancel();
  else if (request.active) request.active(true);
  // Every accepted detection owns a receive reservation, including cancelled
  // file selection. Joining it returns the stream to the same connection.
  const auto succeeded = co_await terminal_connection_transfer(runtime->connection, request, cancellation);
  finish_service(runtime, TerminalServiceRequest{request}, succeeded);
}

static cardio::promise<void> send_break(TerminalRuntime *runtime, TerminalBreakRequest request) {
  const auto succeeded = co_await terminal_connection_send_break(runtime->connection, request);
  runtime->break_active = false;
  request_frame(runtime);
  if (request.finished) request.finished(succeeded);
}

static bool process_input_policy(TerminalRuntime *runtime) {
  if (runtime->pending_input) {
    const auto &settings = *runtime->pending_input;
    auto config = boxrig_input_config_default();
    config.enter_newline = settings.return_code == TerminalReturnCode::cr ? BOXRIG_NEWLINE_CR :
        settings.return_code == TerminalReturnCode::lf ? BOXRIG_NEWLINE_LF :
        settings.return_code == TerminalReturnCode::crlf ? BOXRIG_NEWLINE_CRLF : BOXRIG_NEWLINE_TERMINAL;
    config.backspace_binding = settings.backspace_code == TerminalBackspaceCode::bs ? BOXRIG_ERASE_ASCII_BACKSPACE :
        settings.backspace_code == TerminalBackspaceCode::del ? BOXRIG_ERASE_ASCII_DELETE : BOXRIG_ERASE_AUTO;
    const std::array<uint8_t, 4> bytes{0x1e, 0x1f, 0x1c, 0x1d};
    const std::array<boxrig_key_mapping, 4> mappings{{
        {BOXRIG_KEY_UP, 0, &bytes[0], 1}, {BOXRIG_KEY_DOWN, 0, &bytes[1], 1},
        {BOXRIG_KEY_RIGHT, 0, &bytes[2], 1}, {BOXRIG_KEY_LEFT, 0, &bytes[3], 1}}};
    if (settings.cursor_key_mode == TerminalCursorKeyMode::trs80) {
      config.key_mappings = mappings.data();
      config.key_mapping_count = mappings.size();
    }
    const auto status = boxrig_gtk3_terminal_widget_set_input_config(runtime->widget, &config);
    if (status != BOXRIG_UI_BUSY) {
      if (status == BOXRIG_UI_OK) runtime->applied_input = settings;
      runtime->pending_input.reset();
      if (!ui_result(status, "input settings")) return false;
    }
  }
  const auto connection = terminal_connection_state(runtime->connection);
  if (runtime->auto_selecting && connection.receive_ended) terminal_runtime_cancel_transfer(runtime);
  const bool enabled = runtime->input_allowed && !runtime->pending_connection &&
      !runtime->pending_encoding && !runtime->encoding_active &&
      !runtime->pending_service && !runtime->service_active &&
      (connection.generation == 0 ||
      connection.phase == TerminalConnectionPhase::connected);
  boxrig_ui_input_state input{};
  if (!ui_result(boxrig_gtk3_terminal_widget_get_input_state(runtime->widget, &input), "input state")) return false;
  if ((enabled && input.phase != BOXRIG_UI_INPUT_ENABLED) ||
      (!enabled && input.phase == BOXRIG_UI_INPUT_ENABLED)) {
    const auto status = boxrig_gtk3_terminal_widget_set_input_enabled(runtime->widget, enabled);
    if (status != BOXRIG_UI_BUSY && !ui_result(status, "input inhibition")) return false;
    if (!ui_result(boxrig_gtk3_terminal_widget_get_input_state(runtime->widget, &input), "input state")) return false;
  }
  if (runtime->pending_connection && input.phase == BOXRIG_UI_INPUT_DISABLED) {
    auto command = std::move(*runtime->pending_connection);
    runtime->pending_connection.reset();
    if (!std::visit([runtime](auto request) {
      if constexpr (std::is_same_v<decltype(request), TerminalLocalCommand>)
        return terminal_connection_start_local(runtime->connection, std::move(request));
      else return terminal_connection_start_transport(runtime->connection, std::move(request));
    }, std::move(command))) return false;
  }
  if (runtime->pending_auto_start && input.phase == BOXRIG_UI_INPUT_DISABLED) {
    auto selection = std::move(*runtime->pending_auto_start);
    runtime->pending_auto_start.reset();
    runtime->service_completion.emplace(run_auto_start(runtime, std::move(selection)));
  }
  if (runtime->pending_encoding && input.phase == BOXRIG_UI_INPUT_DISABLED) {
    auto encoding = std::move(*runtime->pending_encoding);
    runtime->pending_encoding.reset();
    runtime->encoding_active = true;
    runtime->encoding_completion.emplace(change_encoding(runtime, std::move(encoding)));
  }
  if (runtime->pending_service && input.phase == BOXRIG_UI_INPUT_DISABLED) {
    auto request = std::move(*runtime->pending_service);
    runtime->pending_service.reset();
    runtime->service_active = true;
    runtime->service_completion.emplace(run_service(runtime, std::move(request)));
  }
  return true;
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

static cardio::promise<void> resize_grid(TerminalRuntime *runtime, boxrig_ui_layout_action action) {
  const auto &grid = action.value.grid_resize;
  const bool resized = co_await terminal_connection_resize(runtime->connection, grid.rows, grid.columns);
  runtime->grid_pending = false;
  if (!ui_result(boxrig_gtk3_terminal_widget_complete_grid_resize(
      runtime->widget, action.revision, resized), "complete grid") || !resized) {
    fail_ui(runtime);
  }
  request_frame(runtime);
}

static bool process_layout(TerminalRuntime *runtime) {
  if (runtime->grid_pending) return true;
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
    runtime->grid_pending = true;
    runtime->grid_completion.emplace(resize_grid(runtime, action));
    return true;
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
  bool consumed = false;
  for (unsigned int work = 0; work < 64 && boxrig_terminal_host_item_peek(runtime->terminal, &item); ++work) {
    if (item.type == BOXRIG_HOST_ITEM_TITLE) {
      const std::string title(item.payload_length == 0 ? "" :
          reinterpret_cast<const char *>(item.payload), item.payload_length);
      if (g_utf8_validate(title.c_str(), title.size(), nullptr)) gtk_window_set_title(runtime->window, title.c_str());
    }
    const auto request_id = item.request_id;
    if (boxrig_terminal_host_item_consume(runtime->terminal) != BOXRIG_STATUS_OK) return false;
    consumed = true;
    if (request_id != 0 && boxrig_terminal_complete_host_request(runtime->terminal,
        request_id, BOXRIG_HOST_REQUEST_UNAVAILABLE, nullptr, 0) != BOXRIG_STATUS_OK) return false;
  }
  if (consumed) request_frame(runtime);
  return true;
}

static gboolean dispatch_frame(gpointer context) {
  auto *runtime = static_cast<TerminalRuntime *>(context);
  runtime->frame_source = 0;
  if (runtime->stopped) return G_SOURCE_REMOVE;
  if (terminal_connection_dispatch(runtime->connection)) request_frame(runtime);
  if (!process_host_items(runtime)) {
    fail_ui(runtime);
    return G_SOURCE_REMOVE;
  }
  // A failed presentation still owns live communication work. Continue its
  // session and host dispatch until cancellation has drained and joined it.
  if (runtime->ui_failed) {
    if (runtime->changed) runtime->changed(runtime->changed_context);
    return G_SOURCE_REMOVE;
  }
  if (!process_input_policy(runtime)) {
    fail_ui(runtime);
    return G_SOURCE_REMOVE;
  }
  // Communication and stream finalization continue while the window is hidden.
  if (!runtime->widget_allocated || !gtk_widget_get_mapped(GTK_WIDGET(runtime->widget))) return G_SOURCE_REMOVE;
  boxrig_ui_component_frame_result result{};
  if (!process_layout(runtime) ||
      !ui_result(boxrig_ui_model_synchronize(runtime->model), "synchronize") ||
      !ui_result(boxrig_gtk3_terminal_widget_frame(runtime->widget, &result), "frame") ||
      !process_input_policy(runtime)) {
    fail_ui(runtime);
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

static void terminal_allocated(GtkWidget *, GtkAllocation *, gpointer context) {
  auto *runtime = static_cast<TerminalRuntime *>(context);
  runtime->widget_allocated = true;
  request_frame(runtime);
}

static gboolean window_state_changed(GtkWidget *, GdkEventWindowState *event, gpointer context) {
  auto *runtime = static_cast<TerminalRuntime *>(context);
  const auto flags = event->new_window_state;
  runtime->window_state = (flags & GDK_WINDOW_STATE_FULLSCREEN) ? BOXRIG_UI_WINDOW_FULLSCREEN :
      (flags & GDK_WINDOW_STATE_MAXIMIZED) ? BOXRIG_UI_WINDOW_MAXIMIZED :
      (flags & GDK_WINDOW_STATE_TILED) ? BOXRIG_UI_WINDOW_TILED : BOXRIG_UI_WINDOW_NORMAL;
  if (!runtime->ui_failed)
    ui_result(boxrig_gtk3_terminal_widget_set_window_state(runtime->widget, runtime->window_state), "window state");
  request_frame(runtime);
  return FALSE;
}

static bool attach_widget(TerminalRuntime *runtime) {
  auto *previous_widget = runtime->widget;
  auto *previous_scroll = runtime->scroll;
  auto widget_config = boxrig_gtk3_terminal_widget_config_default();
  widget_config.render_policy = BOXRIG_GTK_RENDER_OPENGL_PREFERRED;
  widget_config.ime_policy = BOXRIG_GTK_IME_AUTO;
  widget_config.frame_request_callback = request_frame;
  widget_config.frame_request_context = runtime;
  BoxrigGtk3TerminalWidget *widget = nullptr;
  if (!ui_result(boxrig_gtk3_terminal_widget_create(runtime->model, &widget_config, &widget), "create widget")) return false;
  g_object_ref_sink(widget);
  auto *scroll = GTK_WIDGET(boxrig_gtk3_terminal_widget_create_scroll_container(widget));
  if (!scroll) {
    gtk_widget_destroy(GTK_WIDGET(widget));
    g_object_unref(widget);
    return false;
  }
  g_object_ref_sink(scroll);
  if (previous_widget) {
    boxrig_font_specification font{};
    boxrig_font_effective_metrics metrics{};
    if (boxrig_gtk3_terminal_widget_get_font_specification(previous_widget, &font) == BOXRIG_UI_OK &&
        boxrig_gtk3_terminal_widget_get_effective_metrics(previous_widget, &metrics) == BOXRIG_FONT_CATALOG_OK) {
      font.size_dip = metrics.requested_size_dip;
      ui_result(boxrig_gtk3_terminal_widget_set_font_specification(widget, &font), "restore font");
    }
    g_signal_handlers_disconnect_by_data(previous_widget, runtime);
    gtk_widget_destroy(GTK_WIDGET(previous_widget));
    g_object_unref(previous_widget);
    gtk_widget_destroy(previous_scroll);
    g_object_unref(previous_scroll);
  }
  runtime->widget = widget;
  runtime->scroll = scroll;
  runtime->size_command = {};
  runtime->selection_drag_active = false;
  runtime->frame_pending = true;
  runtime->widget_allocated = false;
  runtime->pending_input = runtime->applied_input;
  gtk_box_pack_start(runtime->content, scroll, TRUE, TRUE, 0);
  g_signal_connect(widget, "map", G_CALLBACK(mapped), runtime);
  g_signal_connect(widget, "size-allocate", G_CALLBACK(terminal_allocated), runtime);
  size_t rows = 0, columns = 0;
  boxrig_terminal_size(runtime->terminal, &rows, &columns);
  if (!ui_result(boxrig_gtk3_terminal_widget_set_preferred_grid(widget, rows, columns), "preferred grid") ||
      !ui_result(boxrig_gtk3_terminal_widget_set_window_state(widget, runtime->window_state), "window state") ||
      !ui_result(boxrig_gtk3_terminal_widget_set_mouse_router(widget, route_mouse, runtime), "mouse router") ||
      !ui_result(boxrig_gtk3_terminal_widget_set_visible(widget, true), "visibility")) return false;
  runtime->ui_failed = false;
  gtk_widget_show_all(scroll);
  return true;
}

TerminalRuntime *create_terminal_runtime(GtkWindow *window, GtkBox *content) {
  auto *runtime = new (std::nothrow) TerminalRuntime;
  if (!runtime) return nullptr;
  runtime->window = window;
  runtime->content = content;
  auto config = boxrig_terminal_config_default();
  config.rows = 24;
  config.columns = 80;
  if (boxrig_terminal_create(&config, &runtime->terminal) != BOXRIG_STATUS_OK ||
      !ui_result(boxrig_ui_model_create(runtime->terminal, &runtime->model), "create model") ||
      !attach_widget(runtime)) {
    destroy_terminal_runtime(runtime);
    return nullptr;
  }
  runtime->connection = create_terminal_connection(runtime->terminal,
      [runtime] { request_frame(runtime); },
      [runtime](std::span<const unsigned char> raw, std::span<const unsigned char> cooked) {
        if (runtime->output) runtime->output(raw, cooked);
      });
  g_signal_connect(window, "size-allocate", G_CALLBACK(allocated), runtime);
  g_signal_connect(window, "window-state-event", G_CALLBACK(window_state_changed), runtime);
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
  destroy_terminal_connection(runtime->connection);
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

boxrig_ui_status terminal_runtime_set_preferred_grid(TerminalRuntime *runtime, std::size_t rows, std::size_t columns) {
  const auto status = boxrig_gtk3_terminal_widget_set_preferred_grid(runtime->widget, rows, columns);
  if (status == BOXRIG_UI_OK) request_frame(runtime);
  return status;
}

bool terminal_runtime_is_settled(const TerminalRuntime *runtime) {
  boxrig_ui_size_command_state state{};
  return !runtime->stopped && !runtime->ui_failed && !runtime->grid_pending && !runtime->frame_pending && runtime->frame_source == 0 &&
      runtime->size_command.revision == 0 &&
      boxrig_gtk3_terminal_widget_get_size_command_state(runtime->widget, &state) == BOXRIG_UI_OK &&
      state == BOXRIG_UI_SIZE_COMMAND_IDLE;
}

bool terminal_runtime_connect_local(TerminalRuntime *runtime, TerminalLocalCommand command) {
  const auto phase = terminal_connection_state(runtime->connection).phase;
  if (runtime->stopped || runtime->pending_connection || runtime->pending_service || runtime->service_active ||
      (phase != TerminalConnectionPhase::idle && phase != TerminalConnectionPhase::closed)) return false;
  if (runtime->ui_failed && !attach_widget(runtime)) return false;
  runtime->pending_connection = std::move(command);
  runtime->input_allowed = true;
  request_frame(runtime);
  return true;
}

bool terminal_runtime_connect_transport(TerminalRuntime *runtime, TerminalTransportRequest request) {
  const auto phase = terminal_connection_state(runtime->connection).phase;
  if (runtime->stopped || runtime->pending_connection || runtime->pending_service || runtime->service_active ||
      (phase != TerminalConnectionPhase::idle && phase != TerminalConnectionPhase::closed)) return false;
  if (runtime->ui_failed && !attach_widget(runtime)) return false;
  runtime->pending_connection = std::move(request);
  runtime->input_allowed = true;
  request_frame(runtime);
  return true;
}

cardio::promise<void> terminal_runtime_disconnect(TerminalRuntime *runtime) {
  runtime->pending_connection.reset();
  runtime->pending_encoding.reset();
  terminal_runtime_cancel_transfer(runtime);
  runtime->input_allowed = false;
  if (!runtime->ui_failed) {
    const auto status = boxrig_gtk3_terminal_widget_set_input_enabled(runtime->widget, false);
    if (status != BOXRIG_UI_BUSY) ui_result(status, "disconnect input");
  }
  terminal_connection_stop(runtime->connection);
  request_frame(runtime);
  co_await terminal_connection_wait_stopped(runtime->connection);
  if (runtime->encoding_completion) co_await *runtime->encoding_completion;
  if (runtime->service_completion) co_await *runtime->service_completion;
  if (runtime->break_completion) co_await *runtime->break_completion;
  if (runtime->grid_completion) co_await *runtime->grid_completion;
}

TerminalConnectionState terminal_runtime_connection_state(const TerminalRuntime *runtime) {
  return terminal_connection_state(runtime->connection);
}

void terminal_runtime_set_output_callback(TerminalRuntime *runtime,
    std::function<void(std::span<const unsigned char>, std::span<const unsigned char>)> callback) {
  runtime->output = std::move(callback);
}

void terminal_runtime_request_input_settings(TerminalRuntime *runtime, TerminalTextSettings settings) {
  runtime->pending_input = std::move(settings);
  request_frame(runtime);
}

bool terminal_runtime_input_settings_pending(const TerminalRuntime *runtime) { return runtime->pending_input.has_value(); }

bool terminal_runtime_request_encoding(TerminalRuntime *runtime, std::string encoding) {
  if (runtime->stopped || runtime->ui_failed || runtime->encoding_active ||
      runtime->pending_service || runtime->service_active || runtime->break_active ||
      terminal_connection_state(runtime->connection).phase != TerminalConnectionPhase::connected) return false;
  runtime->pending_encoding = std::move(encoding);
  request_frame(runtime);
  return true;
}

bool terminal_runtime_encoding_pending(const TerminalRuntime *runtime) {
  return runtime->pending_encoding.has_value() || runtime->encoding_active;
}

bool terminal_runtime_start_text_send(TerminalRuntime *runtime, TerminalTextSendRequest request) {
  if (runtime->stopped || runtime->ui_failed || runtime->pending_encoding || runtime->encoding_active ||
      runtime->pending_service || runtime->service_active || runtime->break_active ||
      terminal_connection_state(runtime->connection).phase != TerminalConnectionPhase::connected ||
      request.bytes_per_second == 0 || !terminal_text_send_source_is_valid(request.source)) return false;
  request.text_settings = runtime->applied_input;
  request.text_settings.encoding = terminal_connection_state(runtime->connection).encoding;
  runtime->service_stop = {};
  runtime->pending_service = std::move(request);
  const auto &sending = std::get<TerminalTextSendRequest>(*runtime->pending_service);
  if (sending.active) sending.active(true);
  request_frame(runtime);
  return true;
}

bool terminal_runtime_cancel_transfer(TerminalRuntime *runtime) {
  runtime->service_stop.cancel();
  if (runtime->pending_auto_start) {
    auto selection = std::move(*runtime->pending_auto_start);
    runtime->pending_auto_start.reset();
    runtime->service_completion.emplace(run_auto_start(runtime, std::move(selection)));
    return true;
  }
  if (runtime->pending_service) {
    auto request = std::move(*runtime->pending_service);
    runtime->pending_service.reset();
    if (std::holds_alternative<TerminalTransferRequest>(request)) {
      // Even a cancelled reservation must join its binary read boundary.
      // The cancelled token prevents it from starting any protocol operation.
      runtime->service_active = true;
      runtime->service_completion.emplace(run_service(runtime, std::move(request)));
    } else finish_service(runtime, request, false);
    return true;
  }
  if (!runtime->service_active) return false;
  return true;
}

bool terminal_runtime_start_transfer(TerminalRuntime *runtime, TerminalTransferRequest request) {
  if (runtime->stopped || runtime->ui_failed || runtime->pending_encoding || runtime->encoding_active ||
      runtime->pending_service || runtime->service_active || runtime->break_active ||
      (request.direction == TerminalTransferDirection::send && request.source_file_uris.empty()) ||
      !terminal_connection_prepare_transfer(runtime->connection)) return false;
  runtime->service_stop = {};
  runtime->pending_service = std::move(request);
  const auto &transfer = std::get<TerminalTransferRequest>(*runtime->pending_service);
  if (transfer.active) transfer.active(true);
  request_frame(runtime);
  return true;
}

void terminal_runtime_set_zmodem_auto_start(TerminalRuntime *runtime, TerminalZmodemAutoStart select) {
  runtime->auto_start = std::move(select);
  if (!runtime->auto_start) {
    terminal_connection_set_zmodem_auto_start(runtime->connection, {});
    return;
  }
  terminal_connection_set_zmodem_auto_start(runtime->connection, [runtime](TerminalTransferDirection direction) {
    if (runtime->stopped || runtime->ui_failed || runtime->pending_encoding || runtime->encoding_active ||
        runtime->pending_service || runtime->service_active || runtime->break_active || !runtime->auto_start ||
        !terminal_connection_prepare_transfer(runtime->connection)) return false;
    runtime->service_stop = {};
    runtime->service_active = true;
    runtime->auto_selecting = true;
    runtime->pending_auto_start.emplace(direction, runtime->auto_start);
    request_frame(runtime);
    return true;
  });
}

bool terminal_runtime_can_send_break(const TerminalRuntime *runtime) {
  return !runtime->stopped && !runtime->ui_failed && !runtime->pending_encoding && !runtime->encoding_active &&
      !runtime->pending_service && !runtime->service_active && !runtime->break_active &&
      terminal_connection_can_send_break(runtime->connection);
}

bool terminal_runtime_send_break(TerminalRuntime *runtime, TerminalBreakRequest request) {
  if (!terminal_runtime_can_send_break(runtime)) return false;
  runtime->break_active = true;
  runtime->break_completion.emplace(send_break(runtime, std::move(request)));
  request_frame(runtime);
  return true;
}

void terminal_runtime_request_input_enabled(TerminalRuntime *runtime, bool enabled) {
  runtime->input_allowed = enabled;
  request_frame(runtime);
}

}
