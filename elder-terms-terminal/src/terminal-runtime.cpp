#include "terminal-runtime.h"
#include "terminal-links.h"
#include "terminal-received-macros.h"
#include "terminal-sessions/terminal-text-stream.h"

#include <cstdint>
#include <limits>
#include <new>
#include <array>
#include <algorithm>
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
  std::unique_ptr<TerminalLinks, decltype(&destroy_terminal_links)> links{nullptr, destroy_terminal_links};
  std::function<void(TerminalHyperlinkResolution)> activate_link;
  std::optional<std::pair<boxrig_ui_fixed, boxrig_ui_fixed>> pointer_position;
  bool link_hover = false;
  bool osc8_hover = false;
  bool links_pending = false;
  std::unique_ptr<TerminalReceivedMacros, decltype(&destroy_terminal_received_macros)> macros{
      nullptr, destroy_terminal_received_macros};
  bool macros_pending = false;
  std::function<void(std::span<const unsigned char>, std::span<const unsigned char>)> output;
  std::optional<std::variant<TerminalLocalCommand, TerminalTransportRequest>> pending_connection;
  std::optional<TerminalTransportRequest> recovery_request;
  std::optional<cardio::promise<void>> recovery_completion;
  cardio::cancellation_source recovery_stop;
  std::uint64_t recovery_epoch = 0;
  std::uint64_t attempted_recovery_epoch = 0;
  bool pending_recovery = false;
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
  std::uint64_t handled_carrier_losses = 0;
  BoxrigGtk3TerminalWidget *widget = nullptr;
  GtkWidget *scroll = nullptr;
  GtkWindow *window = nullptr;
  GtkBox *content = nullptr;
  GtkBox *attachment = nullptr;
  TerminalRuntimeHostCallbacks host;
  std::string title;
  bool has_title = false;
  std::vector<std::string> title_stack;
  guint frame_source = 0;
  int frame_priority = G_PRIORITY_DEFAULT;
  bool widget_frame_requested = true;
  guint presentation_source = 0;
  bool stopped = false;
  bool ui_failed = false;
  bool widget_allocated = false;
  bool notified_settled = false;
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
  std::function<void()> prepare_frame;
};

static void request_frame(void *context);
static void request_continuation(TerminalRuntime *runtime);

static void reset_macros(TerminalRuntime *runtime) {
  if (runtime->macros) terminal_received_macros_reset(runtime->macros.get(),
      terminal_connection_state(runtime->connection).generation);
  runtime->macros_pending = false;
  terminal_connection_set_receive_delivery_enabled(runtime->connection, true);
}

static void notify_changed(TerminalRuntime *runtime) {
  runtime->notified_settled = terminal_runtime_is_settled(runtime);
  if (runtime->changed) runtime->changed(runtime->changed_context);
}

static gboolean observe_presentation(gpointer context) {
  auto *runtime = static_cast<TerminalRuntime *>(context);
  runtime->presentation_source = 0;
  if (!runtime->stopped && runtime->notified_settled != terminal_runtime_is_settled(runtime))
    notify_changed(runtime);
  return G_SOURCE_REMOVE;
}

static gboolean drawn(GtkWidget *, cairo_t *, gpointer context) {
  auto *runtime = static_cast<TerminalRuntime *>(context);
  // OpenGL may finish in GTK's draw handler without another boxrig frame
  // request. Observe that native completion once, outside the draw stack;
  // this does not render again, poll, or fabricate a layout completion.
  if (!runtime->stopped && !runtime->presentation_source)
    runtime->presentation_source = g_idle_add(observe_presentation, runtime);
  return FALSE;
}

static void update_link_cursor(TerminalRuntime *runtime, const boxrig_ui_hit_test_result &hit) {
  const auto resolved = runtime->links ? terminal_links_resolve(runtime->links.get(), hit) : std::nullopt;
  const bool active = resolved && resolved->action.has_value();
  auto *view = boxrig_gtk3_terminal_widget_view(runtime->widget);
  if (active && hit.hyperlink_handle) {
    runtime->osc8_hover = boxrig_ui_view_update_hyperlink_hover(view, &hit) == BOXRIG_UI_OK;
  } else if (runtime->osc8_hover) {
    boxrig_ui_view_clear_hyperlink_hover(view);
    runtime->osc8_hover = false;
  }
  if (active != runtime->link_hover) {
    runtime->link_hover = active;
    if (auto *window = gtk_widget_get_window(GTK_WIDGET(runtime->widget))) {
      auto *cursor = gdk_cursor_new_from_name(gdk_window_get_display(window), active ? "pointer" : "text");
      gdk_window_set_cursor(window, cursor);
      if (cursor) g_object_unref(cursor);
    }
    request_frame(runtime);
  }
}

static gboolean pointer_left(GtkWidget *, GdkEventCrossing *, gpointer context) {
  auto *runtime = static_cast<TerminalRuntime *>(context);
  runtime->pointer_position.reset();
  update_link_cursor(runtime, {});
  return FALSE;
}

static boxrig_input_mouse_route route_mouse(
    const boxrig_mouse_event *event, const boxrig_ui_hit_test_result *hit,
    void *context) {
  auto *runtime = static_cast<TerminalRuntime *>(context);
  auto *view = boxrig_gtk3_terminal_widget_view(runtime->widget);
  if (runtime->host.mouse) {
    const auto route = runtime->host.mouse(event, hit);
    if (route != BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL) return route;
  }
  if (runtime->links) {
    runtime->pointer_position = {hit->x, hit->y};
    update_link_cursor(runtime, *hit);
  }
  const bool release = event->action == BOXRIG_MOUSE_RELEASE &&
                       event->button == BOXRIG_MOUSE_BUTTON_LEFT;
  if (runtime->selection_drag_active) {
    if ((event->action == BOXRIG_MOUSE_MOTION || release) &&
        hit->inside_grid && hit->text_position_valid) {
      const auto status = boxrig_ui_view_update_selection(view, hit);
      // Output or a connection transition can remove the selection between
      // queued pointer events. End that drag without failing native input.
      if (status == BOXRIG_UI_STALE || status == BOXRIG_UI_UNAVAILABLE) runtime->selection_drag_active = false;
      else if (status != BOXRIG_UI_OK) {
        g_warning("boxrig selection update failed: %d", status);
        return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
      }
    }
    if (release) {
      runtime->selection_drag_active = false;
      const boxrig_ui_view_action copy{BOXRIG_UI_VIEW_ACTION_COPY,
          BOXRIG_UI_CLIPBOARD_TARGET_PRIMARY_SELECTION};
      const auto status = boxrig_gtk3_terminal_widget_apply_view_action(runtime->widget, &copy);
      if (status != BOXRIG_UI_OK && status != BOXRIG_UI_UNAVAILABLE && status != BOXRIG_UI_STALE) {
        g_warning("boxrig primary selection failed: %d", status);
        return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
      }
    }
    request_frame(runtime);
    return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
  }
  if (event->action != BOXRIG_MOUSE_PRESS || !hit->inside_grid)
    return BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL;
  if (runtime->links && event->button == BOXRIG_MOUSE_BUTTON_LEFT &&
      (event->modifiers & BOXRIG_MODIFIER_CONTROL)) {
    const auto resolved = terminal_links_resolve(runtime->links.get(), *hit);
    if (resolved) {
      if (runtime->activate_link) runtime->activate_link(*resolved);
      return BOXRIG_INPUT_MOUSE_ROUTE_APPLICATION;
    }
  }
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
    if (status != BOXRIG_UI_OK) {
      g_warning("boxrig selection start failed: %d", status);
      return BOXRIG_INPUT_MOUSE_ROUTE_ERROR;
    }
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
  boxrig_ui_component_diagnostic diagnostic{};
  if (boxrig_gtk3_terminal_widget_get_diagnostic(runtime->widget, &diagnostic) == BOXRIG_UI_OK)
    g_warning("boxrig component failed: reason=%d input=%zu/%zu authority=%d",
        diagnostic.failure_reason, diagnostic.native_input_queue_depth,
        diagnostic.native_input_queue_capacity, diagnostic.layout_authority);
  runtime->ui_failed = true;
  runtime->pending_connection.reset();
  runtime->recovery_request.reset();
  runtime->recovery_stop.cancel();
  runtime->pending_encoding.reset();
  terminal_runtime_cancel_transfer(runtime);
  terminal_connection_stop(runtime->connection);
  boxrig_gtk3_terminal_widget_quiesce(runtime->widget);
  request_frame(runtime);
}

static cardio::promise<void> change_encoding(TerminalRuntime *runtime, std::string encoding) {
  if (co_await terminal_connection_change_encoding(runtime->connection, std::move(encoding))) {
    if (runtime->recovery_request)
      runtime->recovery_request->encoding = terminal_connection_state(runtime->connection).encoding;
  }
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

static cardio::promise<void> watch_availability(TerminalRuntime *runtime,
    TerminalTransportRequest request, cardio::cancellation cancellation) {
  try {
    co_await request.watch_availability([runtime, cancellation] {
      if (cancellation.is_cancellation_requested()) return;
      ++runtime->recovery_epoch;
      request_frame(runtime);
    }, cancellation);
  } catch (const cardio::canceled_exception &) {
  } catch (const std::exception &error) {
    g_printerr("Terminal device monitoring: %s\n", error.what());
  }
  if (!cancellation.is_cancellation_requested()) runtime->recovery_request.reset();
  request_frame(runtime);
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
  const bool carrier_interrupted = runtime->handled_carrier_losses != connection.carrier_losses;
  if (connection.phase != TerminalConnectionPhase::starting &&
      (connection.phase != TerminalConnectionPhase::connected || !connection.carrier_connected || carrier_interrupted))
    runtime->pending_encoding.reset();
  if (!connection.carrier_connected || carrier_interrupted) terminal_runtime_cancel_transfer(runtime);
  if (runtime->auto_selecting && connection.receive_ended) terminal_runtime_cancel_transfer(runtime);
  if (connection.phase == TerminalConnectionPhase::closed && !connection.cancelled && runtime->recovery_request &&
      runtime->recovery_epoch != runtime->attempted_recovery_epoch && !runtime->pending_connection &&
      !runtime->pending_service && !runtime->service_active && !runtime->encoding_active && !runtime->break_active) {
    runtime->pending_connection = *runtime->recovery_request;
    runtime->pending_recovery = true;
    runtime->input_allowed = true;
  }
  const bool enabled = runtime->input_allowed && !carrier_interrupted && !runtime->pending_connection &&
      !runtime->pending_encoding && !runtime->encoding_active &&
      !runtime->pending_service && !runtime->service_active &&
      (connection.generation == 0 ||
      (connection.phase == TerminalConnectionPhase::connected && connection.carrier_connected));
  boxrig_ui_input_state input{};
  if (!ui_result(boxrig_gtk3_terminal_widget_get_input_state(runtime->widget, &input), "input state")) return false;
  if ((enabled && input.phase != BOXRIG_UI_INPUT_ENABLED) ||
      (!enabled && input.phase == BOXRIG_UI_INPUT_ENABLED)) {
    const auto status = boxrig_gtk3_terminal_widget_set_input_enabled(runtime->widget, enabled);
    if (status != BOXRIG_UI_BUSY && !ui_result(status, "input inhibition")) return false;
    if (!ui_result(boxrig_gtk3_terminal_widget_get_input_state(runtime->widget, &input), "input state")) return false;
  }
  if (runtime->pending_connection && input.phase == BOXRIG_UI_INPUT_DISABLED &&
      (runtime->pending_recovery || !runtime->recovery_completion || runtime->recovery_completion->is_ready())) {
    auto command = std::move(*runtime->pending_connection);
    runtime->pending_connection.reset();
    if (!runtime->pending_recovery && std::holds_alternative<TerminalTransportRequest>(command)) {
      const auto &request = std::get<TerminalTransportRequest>(command);
      if (request.watch_availability) {
        runtime->recovery_request = request;
        runtime->recovery_stop = {};
        runtime->recovery_completion.emplace(watch_availability(runtime, request, runtime->recovery_stop.get_cancellation()));
      }
    }
    runtime->pending_recovery = false;
    runtime->attempted_recovery_epoch = runtime->recovery_epoch;
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
  if (carrier_interrupted && input.phase == BOXRIG_UI_INPUT_DISABLED) {
    runtime->handled_carrier_losses = connection.carrier_losses;
    request_frame(runtime);
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

static boxrig_ui_status apply_host_grid(TerminalRuntime *runtime, boxrig_grid_size_request request) {
  if (runtime->ui_failed || !runtime->widget_allocated || runtime->window_state != BOXRIG_UI_WINDOW_NORMAL)
    return BOXRIG_UI_OK;
  auto *widget = GTK_WIDGET(runtime->widget);
  auto *native_window = gtk_widget_get_window(GTK_WIDGET(runtime->window));
  if (!native_window) return BOXRIG_UI_OK;
  auto *monitor = gdk_display_get_monitor_at_window(gtk_widget_get_display(widget), native_window);
  if (!monitor) return BOXRIG_UI_OK;
  boxrig_font_effective_metrics metrics{};
  if (boxrig_gtk3_terminal_widget_get_effective_metrics(runtime->widget, &metrics) != BOXRIG_FONT_CATALOG_OK ||
      metrics.cell_width == 0 || metrics.cell_height == 0) return BOXRIG_UI_OK;
  GdkRectangle workarea{}, frame{};
  gdk_monitor_get_workarea(monitor, &workarea);
  gdk_window_get_frame_extents(native_window, &frame);
  // Subtract the observed non-terminal extent once, including decorations and
  // application controls. Workarea uses logical pixels; cell metrics use output
  // pixels. DISPLAY requests must not apply the backing scale twice.
  const auto extra_width = std::max(0, frame.width - gtk_widget_get_allocated_width(widget));
  const auto extra_height = std::max(0, frame.height - gtk_widget_get_allocated_height(widget));
  if (workarea.width <= extra_width || workarea.height <= extra_height) return BOXRIG_UI_OK;
  const auto scale = static_cast<std::uint64_t>(gtk_widget_get_scale_factor(widget));
  const auto display_columns = std::min<std::uint64_t>(1024,
      static_cast<std::uint64_t>(workarea.width - extra_width) * scale / metrics.cell_width);
  const auto display_rows = std::min<std::uint64_t>(1024,
      static_cast<std::uint64_t>(workarea.height - extra_height) * scale / metrics.cell_height);
  std::size_t rows = 0, columns = 0;
  boxrig_terminal_size(runtime->terminal, &rows, &columns);
  if (request.rows.mode == BOXRIG_GRID_SIZE_EXPLICIT) rows = request.rows.cells;
  else if (request.rows.mode == BOXRIG_GRID_SIZE_DISPLAY) rows = display_rows;
  if (request.columns.mode == BOXRIG_GRID_SIZE_EXPLICIT) columns = request.columns.cells;
  else if (request.columns.mode == BOXRIG_GRID_SIZE_DISPLAY) columns = display_columns;
  // Peer requests may only use the current display's available area and a
  // finite cell budget. Rejection leaves the user's normal-state preference.
  if (rows == 0 || columns == 0 || rows > display_rows || columns > display_columns ||
      rows * columns > 256 * 1024) return BOXRIG_UI_OK;
  const auto status = boxrig_gtk3_terminal_widget_set_preferred_grid(runtime->widget, rows, columns);
  if (status == BOXRIG_UI_OK) request_frame(runtime);
  return status;
}

static void apply_host_title(TerminalRuntime *runtime, std::uint32_t flags, std::string title) {
  if (!(flags & BOXRIG_TITLE_WINDOW)) return;
  const auto operation = flags & (BOXRIG_TITLE_STACK_PUSH | BOXRIG_TITLE_STACK_POP);
  if (operation == (BOXRIG_TITLE_STACK_PUSH | BOXRIG_TITLE_STACK_POP)) return;
  if (operation == BOXRIG_TITLE_STACK_PUSH) {
    if (runtime->title_stack.size() == 10) runtime->title_stack.erase(runtime->title_stack.begin());
    runtime->title_stack.push_back(runtime->title);
    return;
  }
  if (operation == BOXRIG_TITLE_STACK_POP) {
    if (runtime->title_stack.empty()) return;
    title = std::move(runtime->title_stack.back());
    runtime->title_stack.pop_back();
  }
  if (!g_utf8_validate(title.c_str(), title.size(), nullptr)) return;
  runtime->title = std::move(title);
  runtime->has_title = true;
  if (runtime->host.title) runtime->host.title(runtime->title);
  else gtk_window_set_title(runtime->window, runtime->title.c_str());
}

static bool process_host_items(TerminalRuntime *runtime) {
  boxrig_host_item item{};
  bool consumed = false;
  for (unsigned int work = 0; work < 64 && boxrig_terminal_host_item_peek(runtime->terminal, &item); ++work) {
    const auto type = item.type;
    const auto flags = item.flags;
    std::string title;
    if (type == BOXRIG_HOST_ITEM_TITLE && item.payload_length != 0)
      title.assign(reinterpret_cast<const char *>(item.payload), item.payload_length);
    if (item.type == BOXRIG_HOST_ITEM_GRID_RESIZE) {
      const auto status = apply_host_grid(runtime, item.grid_resize);
      if (status == BOXRIG_UI_BUSY) {
        if (consumed) request_frame(runtime);
        return true;
      }
      if (!ui_result(status, "host grid preference")) return false;
    }
    const auto request_id = item.request_id;
    if (boxrig_terminal_host_item_consume(runtime->terminal) != BOXRIG_STATUS_OK) return false;
    consumed = true;
    if (request_id != 0 && boxrig_terminal_complete_host_request(runtime->terminal,
        request_id, BOXRIG_HOST_REQUEST_UNAVAILABLE, nullptr, 0) != BOXRIG_STATUS_OK) return false;
    // Consume borrowed host data before application hooks can enqueue input.
    if (type == BOXRIG_HOST_ITEM_TITLE) apply_host_title(runtime, flags, std::move(title));
    else if (type == BOXRIG_HOST_ITEM_BELL && runtime->host.bell) runtime->host.bell();
  }
  if (consumed) request_frame(runtime);
  return true;
}

static gboolean dispatch_frame(gpointer context) {
  auto *runtime = static_cast<TerminalRuntime *>(context);
  runtime->frame_source = 0;
  if (runtime->stopped) return G_SOURCE_REMOVE;
  const auto connection = terminal_connection_state(runtime->connection);
  if (connection.cancelled || !connection.carrier_connected ||
      runtime->handled_carrier_losses != connection.carrier_losses) reset_macros(runtime);
  if (runtime->macros) {
    const auto progress = terminal_received_macros_dispatch(runtime->macros.get(), 4096);
    runtime->macros_pending = progress.pending;
    terminal_connection_set_receive_delivery_enabled(runtime->connection, !progress.pending);
    if (progress.pending && !progress.blocked) request_continuation(runtime);
  }
  auto progress = terminal_connection_dispatch(runtime->connection);
  // New input can complete a synchronized batch while an earlier OpenGL
  // image is pending. An already-dirty view need not notify us again.
  // Sending keys alone does not need another image or accessible snapshot.
  if (progress.input_received) runtime->widget_frame_requested = true;
  if (progress.dispatch_pending) request_continuation(runtime);
  if (!process_host_items(runtime)) {
    fail_ui(runtime);
    return G_SOURCE_REMOVE;
  }
  // A failed presentation still owns live communication work. Continue its
  // session and host dispatch until cancellation has drained and joined it.
  if (runtime->ui_failed) {
    notify_changed(runtime);
    return G_SOURCE_REMOVE;
  }
  if (!process_input_policy(runtime)) {
    fail_ui(runtime);
    return G_SOURCE_REMOVE;
  }
  runtime->links_pending = runtime->links && terminal_links_dispatch(runtime->links.get(), 4096);
  if (runtime->links_pending) request_continuation(runtime);
  // Communication and stream finalization continue while the window is hidden.
  if (!runtime->widget_allocated || !gtk_widget_get_mapped(GTK_WIDGET(runtime->widget))) return G_SOURCE_REMOVE;
  boxrig_ui_component_frame_result result{};
  if (!process_layout(runtime) ||
      !ui_result(boxrig_ui_model_synchronize(runtime->model), "synchronize")) {
    fail_ui(runtime);
    return G_SOURCE_REMOVE;
  }
  // Stream and recognizer continuations do not necessarily change the view.
  // Widget callbacks and received input request component frames,
  // which also publish accessible text; matching alone does not need one.
  if (runtime->widget_frame_requested || !boxrig_gtk3_terminal_widget_has_presentation(runtime->widget)) {
    if (runtime->prepare_frame) runtime->prepare_frame();
    runtime->widget_frame_requested = false;
    if (!ui_result(boxrig_gtk3_terminal_widget_frame(runtime->widget, &result), "frame") ||
        !process_input_policy(runtime)) {
      fail_ui(runtime);
      return G_SOURCE_REMOVE;
    }
  }
  // Native key and IME dispatch can create output without a visual change.
  // Drain it now; a static Cairo view may have no subsequent paint or frame.
  progress = terminal_connection_dispatch(runtime->connection);
  if (progress.input_received) runtime->widget_frame_requested = true;
  if (progress.dispatch_pending) request_continuation(runtime);
  if (runtime->links && runtime->pointer_position) {
    boxrig_ui_hit_test_result hit{};
    if (boxrig_gtk3_terminal_widget_hit_test(runtime->widget, runtime->pointer_position->first,
        runtime->pointer_position->second, &hit) == BOXRIG_UI_OK) update_link_cursor(runtime, hit);
  }
  // Synchronization and hover updates can request a frame during this pass.
  // Let GTK paint its prepared image before continuing stream/recognition
  // work; only the component's additional_dispatch needs immediate service.
  if (runtime->frame_source && runtime->frame_priority < G_PRIORITY_DEFAULT_IDLE) {
    g_source_remove(runtime->frame_source);
    runtime->frame_source = 0;
    request_continuation(runtime);
  }
  if (result.additional_dispatch) request_frame(runtime);
  notify_changed(runtime);
  return G_SOURCE_REMOVE;
}

static void schedule_frame(TerminalRuntime *runtime, int priority) {
  if (!runtime->stopped && (runtime->frame_source == 0 || priority < runtime->frame_priority)) {
    if (runtime->frame_source) g_source_remove(runtime->frame_source);
    runtime->frame_priority = priority;
    runtime->frame_source = g_idle_add_full(priority, dispatch_frame, runtime, nullptr);
  }
}

static void request_frame(void *context) {
  // Dispatch accepted font/layout requests before GTK's resize priority.
  // The callback still only schedules work; no frame runs recursively.
  auto *runtime = static_cast<TerminalRuntime *>(context);
  runtime->widget_frame_requested = true;
  schedule_frame(runtime, G_PRIORITY_DEFAULT);
}

static void request_continuation(TerminalRuntime *runtime) {
  // Long matching and stream work must yield to GTK allocation and drawing.
  // A native/input notification promotes this source to the normal priority.
  schedule_frame(runtime, G_PRIORITY_DEFAULT_IDLE);
}

static void allocated(GtkWidget *, GtkAllocation *, gpointer context) {
  auto *runtime = static_cast<TerminalRuntime *>(context);
  ++runtime->allocation_generation;
  // The widget requests frames for actual terminal allocation changes. Window
  // chrome can be allocated again with the same grid; only an outstanding host
  // size command needs that notification to determine its accepted outcome.
  if (runtime->size_command.revision != 0) request_frame(runtime);
}

static void mapped(GtkWidget *, gpointer context) {
  auto *runtime = static_cast<TerminalRuntime *>(context);
  // An initial preferred grid can already fit its allocation and therefore
  // need no content-size action. Install native hints on that path as well.
  ui_result(boxrig_gtk3_terminal_widget_set_geometry_hints(runtime->widget, runtime->window, true), "geometry hints");
  request_frame(runtime);
}

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
  if (!runtime->ui_failed) {
    ui_result(boxrig_gtk3_terminal_widget_set_window_state(runtime->widget, runtime->window_state), "window state");
    ui_result(boxrig_gtk3_terminal_widget_set_geometry_hints(runtime->widget, runtime->window, true), "geometry hints");
  }
  request_frame(runtime);
  return FALSE;
}

static bool attach_widget(TerminalRuntime *runtime) {
  auto *previous_widget = runtime->widget;
  auto *previous_scroll = runtime->scroll;
  auto widget_config = boxrig_gtk3_terminal_widget_config_default();
  boxrig_font_specification font{};
  boxrig_font_effective_metrics metrics{};
  if (previous_widget &&
      boxrig_gtk3_terminal_widget_get_font_specification(previous_widget, &font) == BOXRIG_UI_OK &&
      boxrig_gtk3_terminal_widget_get_effective_metrics(previous_widget, &metrics) == BOXRIG_FONT_CATALOG_OK) {
    // The model still has the old view's metrics. Resolve the same font during
    // creation, before the replacement joins the model and claims layout.
    font.size_dip = metrics.requested_size_dip;
    widget_config.initial_font = &font;
  }
  widget_config.render_policy = BOXRIG_GTK_RENDER_OPENGL_PREFERRED;
  widget_config.ime_policy = BOXRIG_GTK_IME_AUTO;
  widget_config.accessibility_enabled = previous_widget &&
      boxrig_gtk3_terminal_widget_get_accessibility_enabled(previous_widget);
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
    if (runtime->links) terminal_links_attach_view(runtime->links.get(), nullptr);
    g_signal_handlers_disconnect_by_data(previous_widget, runtime);
    gtk_widget_destroy(GTK_WIDGET(previous_widget));
    g_object_unref(previous_widget);
    gtk_widget_destroy(previous_scroll);
    g_object_unref(previous_scroll);
  }
  runtime->widget = widget;
  if (runtime->links) terminal_links_attach_view(runtime->links.get(), boxrig_gtk3_terminal_widget_view(widget));
  runtime->pointer_position.reset();
  runtime->link_hover = false;
  runtime->osc8_hover = false;
  runtime->scroll = scroll;
  runtime->size_command = {};
  runtime->selection_drag_active = false;
  runtime->widget_allocated = false;
  runtime->pending_input = runtime->applied_input;
  gtk_box_pack_start(runtime->attachment, scroll, TRUE, TRUE, 0);
  g_signal_connect(widget, "map", G_CALLBACK(mapped), runtime);
  g_signal_connect_after(widget, "draw", G_CALLBACK(drawn), runtime);
  gtk_widget_add_events(GTK_WIDGET(widget), GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(widget, "leave-notify-event", G_CALLBACK(pointer_left), runtime);
  g_signal_connect(widget, "size-allocate", G_CALLBACK(terminal_allocated), runtime);
  size_t rows = 0, columns = 0;
  boxrig_terminal_size(runtime->terminal, &rows, &columns);
  if (!ui_result(boxrig_gtk3_terminal_widget_set_preferred_grid(widget, rows, columns), "preferred grid") ||
      !ui_result(boxrig_gtk3_terminal_widget_set_window_state(widget, runtime->window_state), "window state") ||
      !ui_result(boxrig_gtk3_terminal_widget_set_mouse_router(widget, route_mouse, runtime), "mouse router") ||
      !ui_result(boxrig_gtk3_terminal_widget_set_visible(widget, true), "visibility")) return false;
  runtime->ui_failed = false;
  if (runtime->host.attached) runtime->host.attached(widget);
  gtk_widget_show_all(scroll);
  return true;
}

TerminalRuntime *create_terminal_runtime(GtkWindow *window, GtkBox *content, GtkBox *attachment) {
  auto *runtime = new (std::nothrow) TerminalRuntime;
  if (!runtime) return nullptr;
  runtime->window = window;
  runtime->content = content;
  runtime->attachment = attachment;
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
      [runtime] { request_continuation(runtime); },
      [runtime](std::span<const unsigned char> raw, std::span<const unsigned char> cooked) {
        if (runtime->output) runtime->output(raw, cooked);
        const auto state = terminal_connection_state(runtime->connection);
        if (runtime->macros && !cooked.empty() && !state.cancelled && state.carrier_connected) {
          if (!terminal_received_macros_feed(runtime->macros.get(), state.generation, cooked)) {
            g_warning("Received macro queue exceeded its bounded receive window");
            terminal_connection_stop(runtime->connection);
          } else {
            runtime->macros_pending = true;
            terminal_connection_set_receive_delivery_enabled(runtime->connection, false);
            request_frame(runtime);
          }
        }
      });
  g_signal_connect(window, "size-allocate", G_CALLBACK(allocated), runtime);
  g_signal_connect(window, "window-state-event", G_CALLBACK(window_state_changed), runtime);
  return runtime;
}

void destroy_terminal_runtime(TerminalRuntime *runtime) {
  if (!runtime) return;
  runtime->stopped = true;
  runtime->links.reset();
  runtime->macros.reset();
  if (runtime->frame_source) g_source_remove(runtime->frame_source);
  if (runtime->presentation_source) g_source_remove(runtime->presentation_source);
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

void terminal_runtime_set_host_callbacks(TerminalRuntime *runtime, TerminalRuntimeHostCallbacks callbacks) {
  runtime->host = std::move(callbacks);
  if (runtime->host.attached) runtime->host.attached(runtime->widget);
  if (runtime->has_title && runtime->host.title) runtime->host.title(runtime->title);
}

void terminal_runtime_set_hyperlink_rules(TerminalRuntime *runtime,
    std::vector<HyperlinkActionRule> rules, std::function<void(TerminalHyperlinkResolution)> activate) {
  runtime->links.reset();
  update_link_cursor(runtime, {});
  if (!rules.empty()) runtime->links.reset(create_terminal_links(
      boxrig_gtk3_terminal_widget_view(runtime->widget), std::move(rules)));
  runtime->activate_link = std::move(activate);
  request_frame(runtime);
}

bool terminal_runtime_has_link_hover(const TerminalRuntime *runtime) { return runtime->link_hover; }

void terminal_runtime_set_macro_rules(TerminalRuntime *runtime, std::vector<MacroRule> rules,
    bool follow_return_code, std::function<void(std::string, std::vector<std::string>)> command,
    std::function<void(std::string)> error) {
  reset_macros(runtime);
  runtime->macros.reset();
  if (!rules.empty()) runtime->macros.reset(create_terminal_received_macros(std::move(rules), {
      [runtime, follow_return_code](std::string text) {
        return normalize_terminal_text_line_endings(text, follow_return_code, runtime->applied_input.return_code);
      },
      [runtime, error](std::span<const unsigned char> bytes) {
        const auto state = terminal_connection_state(runtime->connection);
        // Modal UI inhibits user input; receive-driven replies remain active.
        if (state.phase != TerminalConnectionPhase::connected ||
            !state.carrier_connected || runtime->pending_service || runtime->service_active ||
            runtime->pending_encoding || runtime->encoding_active || runtime->break_active) return true;
        const auto status = boxrig_terminal_send_text(runtime->terminal,
            reinterpret_cast<const char *>(bytes.data()), bytes.size());
        if (status == BOXRIG_STATUS_WOULD_BLOCK) return false;
        if (status != BOXRIG_STATUS_OK && error)
          error("Macro send failed (status " + std::to_string(status) + ")");
        return true;
      }, std::move(command), std::move(error)}));
  request_frame(runtime);
}

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

void terminal_runtime_set_prepare_frame_callback(TerminalRuntime *runtime, std::function<void()> callback) {
  runtime->prepare_frame = std::move(callback);
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
  boxrig_gtk_render_state render{};
  return !runtime->stopped && !runtime->ui_failed && !runtime->links_pending && !runtime->macros_pending &&
      !runtime->grid_pending && runtime->frame_source == 0 && !runtime->widget_frame_requested &&
      runtime->size_command.revision == 0 &&
      boxrig_gtk3_terminal_widget_has_presentation(runtime->widget) &&
      !boxrig_ui_view_frame_requested(boxrig_gtk3_terminal_widget_view(runtime->widget)) &&
      boxrig_gtk3_terminal_widget_get_render_state(runtime->widget, &render) == BOXRIG_UI_OK && !render.pending &&
      boxrig_gtk3_terminal_widget_get_size_command_state(runtime->widget, &state) == BOXRIG_UI_OK &&
      state == BOXRIG_UI_SIZE_COMMAND_IDLE;
}

bool terminal_runtime_connect_local(TerminalRuntime *runtime, TerminalLocalCommand command) {
  const auto phase = terminal_connection_state(runtime->connection).phase;
  if (runtime->stopped || runtime->pending_connection || runtime->pending_service || runtime->service_active ||
      (phase != TerminalConnectionPhase::idle && phase != TerminalConnectionPhase::closed)) return false;
  if (runtime->ui_failed && !attach_widget(runtime)) return false;
  runtime->recovery_request.reset();
  runtime->recovery_stop.cancel();
  runtime->pending_recovery = false;
  runtime->pending_connection = std::move(command);
  runtime->input_allowed = true;
  request_frame(runtime);
  return true;
}

void terminal_runtime_refresh(TerminalRuntime *runtime) {
  request_frame(runtime);
}

bool terminal_runtime_connect_transport(TerminalRuntime *runtime, TerminalTransportRequest request) {
  const auto phase = terminal_connection_state(runtime->connection).phase;
  if (runtime->stopped || runtime->pending_connection || runtime->pending_service || runtime->service_active ||
      (phase != TerminalConnectionPhase::idle && phase != TerminalConnectionPhase::closed)) return false;
  if (runtime->ui_failed && !attach_widget(runtime)) return false;
  runtime->recovery_request.reset();
  runtime->recovery_stop.cancel();
  runtime->pending_recovery = false;
  runtime->pending_connection = std::move(request);
  runtime->input_allowed = true;
  request_frame(runtime);
  return true;
}

cardio::promise<void> terminal_runtime_disconnect(TerminalRuntime *runtime) {
  runtime->pending_connection.reset();
  runtime->recovery_request.reset();
  runtime->recovery_stop.cancel();
  runtime->pending_encoding.reset();
  terminal_runtime_cancel_transfer(runtime);
  runtime->input_allowed = false;
  if (!runtime->ui_failed) {
    const auto status = boxrig_gtk3_terminal_widget_set_input_enabled(runtime->widget, false);
    if (status != BOXRIG_UI_BUSY) ui_result(status, "disconnect input");
  }
  terminal_connection_stop(runtime->connection);
  reset_macros(runtime);
  request_frame(runtime);
  co_await terminal_connection_wait_stopped(runtime->connection);
  if (runtime->recovery_completion) co_await *runtime->recovery_completion;
  if (runtime->encoding_completion) co_await *runtime->encoding_completion;
  if (runtime->service_completion) co_await *runtime->service_completion;
  if (runtime->break_completion) co_await *runtime->break_completion;
  if (runtime->grid_completion) co_await *runtime->grid_completion;
}

TerminalConnectionState terminal_runtime_connection_state(const TerminalRuntime *runtime) {
  return terminal_connection_state(runtime->connection);
}

std::shared_ptr<AuthenticatedSshTransport> terminal_runtime_authenticated_ssh(const TerminalRuntime *runtime) {
  return terminal_connection_authenticated_ssh(runtime->connection);
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
      runtime->pending_service || runtime->service_active || runtime->break_active) return false;
  const auto connection = terminal_connection_state(runtime->connection);
  if (runtime->pending_connection ||
      (connection.phase == TerminalConnectionPhase::closed && runtime->recovery_request)) {
    try {
      const auto decoder = create_terminal_text_decoder(encoding);
      const auto encoder = create_terminal_text_encoder(encoding);
    } catch (const std::system_error &) { return false; }
    if (runtime->pending_connection)
      std::visit([&encoding](auto &request) { request.encoding = encoding; }, *runtime->pending_connection);
    if (runtime->recovery_request) runtime->recovery_request->encoding = std::move(encoding);
    return true;
  }
  if (connection.phase != TerminalConnectionPhase::starting &&
      (connection.phase != TerminalConnectionPhase::connected || !connection.carrier_connected)) return false;
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
      !terminal_connection_state(runtime->connection).carrier_connected ||
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
