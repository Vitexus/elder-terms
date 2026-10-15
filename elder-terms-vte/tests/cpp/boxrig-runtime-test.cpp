#include "../../src/terminal-runtime.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

using namespace elder_terms;

struct Fixture {
  TerminalRuntime *runtime = nullptr;
  GtkWidget *window = nullptr;
  cardio::primitives::conditional changed;
  cardio::primitives::conditional received;
  cardio::primitives::conditional resize_ready;
  cardio::primitives::conditional write_ready;
  cardio::primitives::conditional break_ready;
  cardio::primitives::conditional selection_ready;
  cardio::primitives::conditional availability;
  std::function<void()> notify_available;
  bool monitoring = false;
  bool device_available = false;
  std::string incoming;
  std::string outgoing;
  std::string normal_output;
  std::string transfer_directory;
  int services_finished = 0;
  bool service_succeeded = false;
  bool resize_pending = false;
  bool fail_resize = false;
  bool hold_resize = true;
  bool closed = false;
  bool hold_write = false;
  bool write_pending = false;
  bool break_asserted = false;
  bool closed_during_break = false;
  bool selection_pending = false;
  bool selection_cancelled = false;
  bool peer_eof = false;
  std::size_t requested_rows = 0;
  std::size_t requested_columns = 0;
};

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static std::string text(Fixture *fixture) {
  auto *accessible = gtk_widget_get_accessible(GTK_WIDGET(terminal_runtime_widget(fixture->runtime)));
  auto *value = atk_text_get_text(ATK_TEXT(accessible), 0, -1);
  std::string result(value);
  g_free(value);
  return result;
}

static cardio::promise<std::size_t> read_peer(Fixture *fixture, std::span<unsigned char> bytes,
    cardio::cancellation cancellation) {
  while (fixture->incoming.empty() && !fixture->peer_eof) co_await fixture->received.wait(cancellation);
  const auto count = std::min(bytes.size(), fixture->incoming.size());
  std::memcpy(bytes.data(), fixture->incoming.data(), count);
  fixture->incoming.erase(0, count);
  fixture->changed.trigger();
  co_return count;
}

static cardio::promise<void> resize_peer(Fixture *fixture, std::size_t rows, std::size_t columns,
    cardio::cancellation cancellation) {
  fixture->resize_pending = true;
  fixture->requested_rows = rows;
  fixture->requested_columns = columns;
  fixture->changed.trigger();
  if (fixture->hold_resize) co_await fixture->resize_ready.wait(cancellation);
  fixture->resize_pending = false;
  if (fixture->fail_resize) throw std::system_error(EIO, std::generic_category(), "peer resize failed");
}

static TerminalTransportRequest request(Fixture *fixture) {
  TerminalTransportRequest request;
  request.open = [fixture](std::size_t, std::size_t, cardio::cancellation cancellation) -> cardio::promise<TerminalTransport> {
    cancellation.throw_if_cancellation_requested();
    fixture->closed = false;
    fixture->peer_eof = false;
    co_return TerminalTransport{
        [fixture](std::span<unsigned char> bytes, cardio::cancellation stop) { return read_peer(fixture, bytes, stop); },
        [fixture](std::span<const unsigned char> bytes, cardio::cancellation stop) -> cardio::promise<void> {
          stop.throw_if_cancellation_requested();
          fixture->outgoing.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
          fixture->write_pending = true;
          fixture->changed.trigger();
          try {
            if (fixture->hold_write) co_await fixture->write_ready.wait(stop);
          } catch (...) {
            fixture->write_pending = false;
            fixture->changed.trigger();
            throw;
          }
          fixture->write_pending = false;
          fixture->changed.trigger();
          co_return;
        },
        [fixture](std::size_t rows, std::size_t columns, cardio::cancellation stop) { return resize_peer(fixture, rows, columns, stop); },
        [fixture]() -> cardio::promise<void> {
          fixture->closed_during_break = fixture->break_asserted;
          fixture->closed = true;
          co_return;
        }, {},
        [fixture](cardio::cancellation stop) -> cardio::promise<void> {
          fixture->break_asserted = true;
          fixture->changed.trigger();
          try { co_await fixture->break_ready.wait(stop); }
          catch (...) { fixture->break_asserted = false; throw; }
          fixture->break_asserted = false;
        }};
  };
  return request;
}

static cardio::promise<void> scenarios(Fixture *fixture) {
  while (!terminal_runtime_is_settled(fixture->runtime)) co_await fixture->changed.wait();
  expect(terminal_runtime_connect_transport(fixture->runtime, request(fixture)), "connect channel");
  while (terminal_runtime_connection_state(fixture->runtime).phase != TerminalConnectionPhase::connected)
    co_await fixture->changed.wait();
  fixture->incoming = "RETAINED DISPLAY\r\n";
  fixture->received.trigger();
  while (text(fixture).find("RETAINED DISPLAY") == std::string::npos) co_await fixture->changed.wait();
  auto *widget = terminal_runtime_widget(fixture->runtime);
  fixture->incoming = "\033[8;30;100t";
  fixture->received.trigger();
  while (!fixture->resize_pending) co_await fixture->changed.wait();
  expect(fixture->requested_rows == 30 && fixture->requested_columns == 100, "host grid request reaches the peer through layout");
  boxrig_ui_layout_result layout{};
  fixture->incoming = "\033[999;999H\033[6n\033[2;1H";
  fixture->received.trigger();
  while (fixture->outgoing.find('R') == std::string::npos) co_await fixture->changed.wait();
  expect(fixture->outgoing == "\033[24;80R", "terminal replies use the confirmed grid while peer resize is pending");
  fixture->incoming = "\033[8;;110t";
  fixture->received.trigger();
  std::size_t preferred_rows = 0, preferred_columns = 0;
  do {
    co_await fixture->changed.wait();
    boxrig_gtk3_terminal_widget_get_preferred_grid(widget, &preferred_rows, &preferred_columns);
  } while (preferred_columns != 110);
  expect(preferred_rows == 24, "omitted rows retain the confirmed core grid while a resize is pending");
  fixture->resize_ready.trigger();
  while (fixture->requested_columns != 110) co_await fixture->changed.wait();
  fixture->resize_ready.trigger();
  while (!terminal_runtime_is_settled(fixture->runtime)) co_await fixture->changed.wait();
  expect(boxrig_gtk3_terminal_widget_get_layout(widget, &layout) == BOXRIG_UI_OK && layout.columns == 110 && layout.rows == 24,
      "later grid completes after its own peer request");

  fixture->hold_resize = false;
  fixture->incoming = "\033[8;0;0t";
  fixture->received.trigger();
  while (fixture->requested_columns <= 110 || fixture->requested_rows <= 24 ||
      !terminal_runtime_is_settled(fixture->runtime)) co_await fixture->changed.wait();
  boxrig_gtk3_terminal_widget_get_preferred_grid(widget, &preferred_rows, &preferred_columns);
  expect(preferred_columns > 110 && preferred_rows > 24, "zero dimensions expand to the display instead of retaining the old grid");
  fixture->incoming = "\033[8;4294967295;4294967295tREJECTED_SIZE";
  fixture->received.trigger();
  while (text(fixture).find("REJECTED_SIZE") == std::string::npos || !terminal_runtime_is_settled(fixture->runtime))
    co_await fixture->changed.wait();
  std::size_t retained_rows = 0, retained_columns = 0;
  boxrig_gtk3_terminal_widget_get_preferred_grid(widget, &retained_rows, &retained_columns);
  expect(preferred_rows == retained_rows && preferred_columns == retained_columns, "unbounded peer dimensions are rejected without changing the preferred grid");

  for (const auto state : {GDK_WINDOW_STATE_MAXIMIZED, GDK_WINDOW_STATE_TILED, GDK_WINDOW_STATE_FULLSCREEN}) {
    GdkEventWindowState event{};
    event.type = GDK_WINDOW_STATE;
    event.new_window_state = state;
    gboolean handled = FALSE;
    g_signal_emit_by_name(fixture->window, "window-state-event", &event, &handled);
    const auto marker = "CONSTRAINED_" + std::to_string(static_cast<int>(state));
    fixture->incoming = "\033[8;29;90t\r\n" + marker;
    fixture->received.trigger();
    while (text(fixture).find(marker) == std::string::npos || !terminal_runtime_is_settled(fixture->runtime))
      co_await fixture->changed.wait();
    boxrig_gtk3_terminal_widget_get_preferred_grid(widget, &retained_rows, &retained_columns);
    expect(preferred_rows == retained_rows && preferred_columns == retained_columns,
        "constrained window states reject peer requests without replacing the normal grid");
  }
  GdkEventWindowState normal{};
  normal.type = GDK_WINDOW_STATE;
  gboolean handled = FALSE;
  g_signal_emit_by_name(fixture->window, "window-state-event", &normal, &handled);
  while (!terminal_runtime_is_settled(fixture->runtime)) co_await fixture->changed.wait();

  fixture->hold_resize = true;
  fixture->fail_resize = true;
  expect(terminal_runtime_set_preferred_grid(fixture->runtime, 34, 120) == BOXRIG_UI_OK, "request failing grid");
  while (!fixture->resize_pending) co_await fixture->changed.wait();
  fixture->resize_ready.trigger();
  while (terminal_runtime_connection_state(fixture->runtime).phase != TerminalConnectionPhase::closed)
    co_await fixture->changed.wait();
  expect(fixture->closed && terminal_runtime_connection_state(fixture->runtime).operation_error == EIO,
      "peer resize failure shuts down and joins the channel");
  expect(text(fixture).find("RETAINED DISPLAY") != std::string::npos, "failure retains displayed text");

  fixture->fail_resize = false;
  fixture->hold_resize = false;
  expect(terminal_runtime_connect_transport(fixture->runtime, request(fixture)), "reconnect after widget quiescence");
  while (terminal_runtime_connection_state(fixture->runtime).phase != TerminalConnectionPhase::connected)
    co_await fixture->changed.wait();
  fixture->incoming = "RECONNECTED\r\n";
  fixture->received.trigger();
  while (text(fixture).find("RECONNECTED") == std::string::npos) co_await fixture->changed.wait();
  expect(text(fixture).find("RETAINED DISPLAY") != std::string::npos, "new widget uses the same core and history");

  fixture->outgoing.clear();
  fixture->hold_write = true;
  fixture->incoming = "\033[H\033[6n";
  fixture->received.trigger();
  while (!fixture->write_pending) co_await fixture->changed.wait();
  expect(fixture->outgoing == "\033[1;1R", "normal reply is held in the peer writer");
  auto &finished = fixture->services_finished;
  auto &succeeded = fixture->service_succeeded;
  TerminalTextSendRequest sending;
  sending.source = TerminalTextSendBufferSource{"TEXT"};
  sending.bytes_per_second = 1000000;
  sending.finished = [fixture](bool success) {
    ++fixture->services_finished;
    fixture->service_succeeded = success;
    fixture->changed.trigger();
  };
  expect(terminal_runtime_start_text_send(fixture->runtime, sending), "accept text send while normal output drains");
  boxrig_ui_input_state input{};
  do {
    co_await fixture->changed.wait();
    boxrig_gtk3_terminal_widget_get_input_state(terminal_runtime_widget(fixture->runtime), &input);
  } while (input.phase != BOXRIG_UI_INPUT_DISABLED);
  expect(fixture->outgoing == "\033[1;1R", "text cannot overtake the pending normal write");
  fixture->write_ready.trigger();
  while (fixture->outgoing != "\033[1;1RTEXT") co_await fixture->changed.wait();
  expect(!terminal_runtime_request_encoding(fixture->runtime, "CP932"), "encoding exchange waits for a text send to end");
  expect(finished == 0, "text send completion waits for peer write completion");
  fixture->incoming = "RECEIVED DURING TEXT SEND\r\n\033[H\033[6n";
  fixture->received.trigger();
  while (text(fixture).find("RECEIVED DURING TEXT SEND") == std::string::npos) co_await fixture->changed.wait();
  expect(fixture->outgoing == "\033[1;1RTEXT", "late terminal replies do not interleave with text-send bytes");
  fixture->hold_write = false;
  fixture->write_ready.trigger();
  while (finished != 1 || fixture->outgoing != "\033[1;1RTEXT\033[1;1R") co_await fixture->changed.wait();
  expect(succeeded, "text send succeeds once and normal replies resume");

  fixture->hold_write = true;
  expect(terminal_runtime_start_text_send(fixture->runtime, sending), "start cancellable text send");
  while (!fixture->write_pending) co_await fixture->changed.wait();
  expect(terminal_runtime_cancel_transfer(fixture->runtime), "cancel active text send");
  while (finished != 2) co_await fixture->changed.wait();
  expect(!succeeded && !fixture->closed, "text cancellation retains the connection");
  do {
    co_await fixture->changed.wait();
    boxrig_gtk3_terminal_widget_get_input_state(terminal_runtime_widget(fixture->runtime), &input);
  } while (input.phase != BOXRIG_UI_INPUT_ENABLED);
  expect(terminal_runtime_start_text_send(fixture->runtime, sending), "start text send before disconnect");
  while (!fixture->write_pending) co_await fixture->changed.wait();
  co_await terminal_runtime_disconnect(fixture->runtime);
  expect(finished == 3 && !succeeded && fixture->closed, "disconnect joins an active text send before closing the channel");

  fixture->hold_write = false;
  fixture->outgoing.clear();
  auto binary_request = request(fixture);
  binary_request.queue_capacity = 17;
  binary_request.io_chunk_size = 3;
  expect(terminal_runtime_connect_transport(fixture->runtime, std::move(binary_request)), "connect for bounded binary routing");
  while (terminal_runtime_connection_state(fixture->runtime).phase != TerminalConnectionPhase::connected)
    co_await fixture->changed.wait();
  TerminalTransferRequest transfer;
  transfer.protocol = TerminalTransferProtocol::xmodem;
  transfer.direction = TerminalTransferDirection::receive;
  transfer.base_path = fixture->transfer_directory;
  transfer.finished = sending.finished;
  expect(terminal_runtime_start_transfer(fixture->runtime, transfer), "reserve binary input before stopping widget input");
  while (fixture->outgoing.empty()) co_await fixture->changed.wait();
  expect(fixture->outgoing == "C", "binary receiver requests CRC after normal output drains");
  boxrig_gtk3_terminal_widget_get_input_state(terminal_runtime_widget(fixture->runtime), &input);
  expect(input.phase == BOXRIG_UI_INPUT_DISABLED, "binary transfer starts only after widget input is disabled");
  const auto normal_size = fixture->normal_output.size();
  fixture->incoming = "\x01\x01\xfe" "BINARY_PAYLOAD_NOT_TEXT";
  fixture->received.trigger();
  while (!fixture->incoming.empty()) co_await fixture->changed.wait();
  expect(terminal_runtime_feed(fixture->runtime, "\033[H\033[6n"), "generate a late normal terminal reply");
  while (!terminal_runtime_is_settled(fixture->runtime)) co_await fixture->changed.wait();
  expect(fixture->outgoing == "C", "normal terminal replies remain blocked during a binary transfer");
  expect(fixture->normal_output.size() == normal_size, "binary input is excluded from normal output notifications");
  expect(terminal_runtime_cancel_transfer(fixture->runtime), "cancel binary transfer");
  while (finished != 4 || !fixture->outgoing.ends_with("\033[1;1R")) co_await fixture->changed.wait();
  expect(!succeeded && !fixture->closed, "binary cancellation resumes normal output on the same connection");

  const auto before_pending_cancel = fixture->outgoing;
  expect(terminal_runtime_start_transfer(fixture->runtime, transfer), "reserve a second binary transfer");
  expect(terminal_runtime_cancel_transfer(fixture->runtime), "cancel before protocol startup");
  while (finished != 5) co_await fixture->changed.wait();
  expect(fixture->outgoing == before_pending_cancel, "cancelled reservation cannot write a protocol handshake");
  expect(terminal_runtime_start_transfer(fixture->runtime, transfer), "start binary transfer before disconnect");
  while (fixture->outgoing == before_pending_cancel) co_await fixture->changed.wait();
  co_await terminal_runtime_disconnect(fixture->runtime);
  expect(finished == 6 && !succeeded && fixture->closed, "disconnect recovers binary receive and transfer work before closing");

  fixture->outgoing.clear();
  fixture->hold_write = true;
  expect(terminal_runtime_connect_transport(fixture->runtime, request(fixture)), "connect for BREAK ordering");
  while (terminal_runtime_connection_state(fixture->runtime).phase != TerminalConnectionPhase::connected)
    co_await fixture->changed.wait();
  fixture->incoming = "\033[H\033[6n";
  fixture->received.trigger();
  while (!fixture->write_pending) co_await fixture->changed.wait();
  TerminalBreakRequest breaking;
  breaking.finished = sending.finished;
  expect(terminal_runtime_send_break(fixture->runtime, breaking), "accept BREAK after pending normal output");
  expect(!fixture->break_asserted, "BREAK cannot overtake an accepted normal write");
  fixture->hold_write = false;
  fixture->write_ready.trigger();
  while (!fixture->break_asserted) co_await fixture->changed.wait();
  expect(!terminal_runtime_start_text_send(fixture->runtime, sending), "text send waits until BREAK finishes");
  expect(!terminal_runtime_request_encoding(fixture->runtime, "CP932"), "encoding exchange waits until BREAK finishes");
  expect(terminal_runtime_feed(fixture->runtime, "\033[H\033[6n"), "generate a reply during BREAK");
  while (!terminal_runtime_is_settled(fixture->runtime)) co_await fixture->changed.wait();
  expect(fixture->outgoing == "\033[1;1R", "normal output remains queued while BREAK is asserted");
  fixture->break_ready.trigger();
  while (finished != 7 || fixture->outgoing != "\033[1;1R\033[1;1R") co_await fixture->changed.wait();
  expect(succeeded, "BREAK completion resumes normal output");
  expect(terminal_runtime_send_break(fixture->runtime, breaking), "start BREAK before disconnect");
  while (!fixture->break_asserted) co_await fixture->changed.wait();
  co_await terminal_runtime_disconnect(fixture->runtime);
  expect(finished == 8 && !succeeded && fixture->closed && !fixture->closed_during_break,
      "disconnect cancels and deasserts BREAK before closing the channel");

  TerminalZmodemAutoStart select = [fixture](TerminalTransferDirection direction, cardio::cancellation cancellation)
      -> cardio::promise<std::optional<TerminalTransferRequest>> {
    expect(direction == TerminalTransferDirection::send, "ZRINIT requests a local send");
    fixture->selection_pending = true;
    fixture->selection_cancelled = false;
    fixture->changed.trigger();
    try { co_await fixture->selection_ready.wait(cancellation); }
    catch (const cardio::canceled_exception &) { fixture->selection_cancelled = true; }
    fixture->selection_pending = false;
    fixture->changed.trigger();
    co_return std::nullopt;
  };
  terminal_runtime_set_zmodem_auto_start(fixture->runtime, select);
  auto automatic_request = request(fixture);
  automatic_request.queue_capacity = 1;
  automatic_request.io_chunk_size = 1;
  expect(terminal_runtime_connect_transport(fixture->runtime, std::move(automatic_request)), "connect for split auto-start detection");
  while (terminal_runtime_connection_state(fixture->runtime).phase != TerminalConnectionPhase::connected)
    co_await fixture->changed.wait();
  fixture->outgoing.clear();
  const auto prior_normal = fixture->normal_output;
  fixture->incoming = "BEFORE**\x18" "B0100000063f694\r";
  fixture->received.trigger();
  while (!fixture->selection_pending || !fixture->incoming.empty()) co_await fixture->changed.wait();
  boxrig_gtk3_terminal_widget_get_input_state(terminal_runtime_widget(fixture->runtime), &input);
  expect(input.phase == BOXRIG_UI_INPUT_DISABLED && fixture->outgoing.empty(),
      "file selection inhibits input before any protocol response");
  expect(fixture->normal_output == prior_normal + "BEFORE", "split preamble and bounded queued bytes stay out of normal output");
  expect(terminal_runtime_cancel_transfer(fixture->runtime), "cancel automatic file selection");
  do {
    co_await fixture->changed.wait();
    boxrig_gtk3_terminal_widget_get_input_state(terminal_runtime_widget(fixture->runtime), &input);
  } while (input.phase != BOXRIG_UI_INPUT_ENABLED);
  expect(fixture->selection_cancelled && !fixture->closed && fixture->outgoing.empty(),
      "selection cancellation joins the reservation without starting the protocol");
  fixture->incoming = "AFTER*";
  fixture->received.trigger();
  while (!fixture->incoming.empty()) co_await fixture->changed.wait();
  terminal_runtime_set_zmodem_auto_start(fixture->runtime, {});
  while (fixture->normal_output != prior_normal + "BEFOREAFTER*") co_await fixture->changed.wait();
  terminal_runtime_set_zmodem_auto_start(fixture->runtime, select);
  fixture->incoming = "**\x18" "B0100000063f694";
  fixture->received.trigger();
  while (!fixture->selection_pending) co_await fixture->changed.wait();
  fixture->peer_eof = true;
  fixture->received.trigger();
  while (terminal_runtime_connection_state(fixture->runtime).phase != TerminalConnectionPhase::closed)
    co_await fixture->changed.wait();
  co_await terminal_runtime_disconnect(fixture->runtime);
  expect(fixture->selection_cancelled && !fixture->selection_pending && fixture->closed,
      "peer EOF cancels and joins automatic selection and its receive reservation");

  expect(terminal_runtime_connect_transport(fixture->runtime, request(fixture)), "reconnect for selection shutdown");
  while (terminal_runtime_connection_state(fixture->runtime).phase != TerminalConnectionPhase::connected)
    co_await fixture->changed.wait();
  fixture->incoming = "**\x18" "B0100000063f694";
  fixture->received.trigger();
  while (!fixture->selection_pending) co_await fixture->changed.wait();
  co_await terminal_runtime_disconnect(fixture->runtime);
  expect(fixture->selection_cancelled && !fixture->selection_pending && fixture->closed,
      "explicit disconnect joins automatic selection before closing");

  expect(terminal_runtime_connect_transport(fixture->runtime, request(fixture)), "reconnect for incomplete header EOF");
  while (terminal_runtime_connection_state(fixture->runtime).phase != TerminalConnectionPhase::connected)
    co_await fixture->changed.wait();
  const auto before_eof = fixture->normal_output;
  fixture->incoming = "**\x18" "B0000";
  fixture->peer_eof = true;
  fixture->received.trigger();
  while (terminal_runtime_connection_state(fixture->runtime).phase != TerminalConnectionPhase::closed)
    co_await fixture->changed.wait();
  expect(fixture->normal_output == before_eof + "**\x18" "B0000", "EOF releases an incomplete header as ordinary data exactly once");

  const auto retained_display = text(fixture);
  auto recovering = request(fixture);
  recovering.open = [fixture, open = recovering.open](std::size_t rows, std::size_t columns, cardio::cancellation stop)
      -> cardio::promise<TerminalTransport> {
    if (!fixture->device_available) throw std::system_error(ENOENT, std::generic_category(), "device is absent");
    co_return co_await open(rows, columns, stop);
  };
  recovering.watch_availability = [fixture](std::function<void()> notify, cardio::cancellation stop) -> cardio::promise<void> {
    fixture->monitoring = true;
    fixture->notify_available = std::move(notify);
    fixture->changed.trigger();
    try { co_await fixture->availability.wait(stop); }
    catch (const cardio::canceled_exception &) {}
    fixture->monitoring = false;
    fixture->notify_available = {};
    fixture->changed.trigger();
  };
  auto generation = terminal_runtime_connection_state(fixture->runtime).generation;
  expect(terminal_runtime_connect_transport(fixture->runtime, recovering), "monitor a temporarily absent device");
  while (terminal_runtime_connection_state(fixture->runtime).generation == generation ||
      terminal_runtime_connection_state(fixture->runtime).phase != TerminalConnectionPhase::closed)
    co_await fixture->changed.wait();
  expect(fixture->monitoring && terminal_runtime_connection_state(fixture->runtime).operation_error == ENOENT,
      "failed open remains observable while device monitoring continues");
  fixture->device_available = true;
  fixture->notify_available();
  while (terminal_runtime_connection_state(fixture->runtime).phase != TerminalConnectionPhase::connected)
    co_await fixture->changed.wait();
  expect(terminal_runtime_request_encoding(fixture->runtime, "CP932"), "change encoding before reattachment");
  while (terminal_runtime_encoding_pending(fixture->runtime)) co_await fixture->changed.wait();
  generation = terminal_runtime_connection_state(fixture->runtime).generation;
  fixture->peer_eof = true;
  fixture->notify_available();
  fixture->received.trigger();
  while (terminal_runtime_connection_state(fixture->runtime).generation == generation ||
      terminal_runtime_connection_state(fixture->runtime).phase != TerminalConnectionPhase::connected)
    co_await fixture->changed.wait();
  expect(terminal_runtime_connection_state(fixture->runtime).encoding == "CP932",
      "recovery retains applied encoding and an event received before teardown");
  expect(text(fixture) == retained_display, "device recovery retains the same terminal history");
  co_await terminal_runtime_disconnect(fixture->runtime);
  expect(!fixture->monitoring && !fixture->notify_available,
      "explicit disconnect joins the monitor and releases its callback");
}

static cardio::promise<void> run(Fixture *fixture, std::exception_ptr *failure) {
  try { co_await scenarios(fixture); }
  catch (...) { *failure = std::current_exception(); }
  co_await terminal_runtime_disconnect(fixture->runtime);
  gtk_main_quit();
}

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  auto *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(window), content);
  Fixture fixture;
  fixture.window = window;
  auto *directory = g_dir_make_tmp("boxrig-runtime-transfer-XXXXXX", nullptr);
  if (!directory) return 1;
  fixture.transfer_directory = directory;
  g_free(directory);
  fixture.runtime = create_terminal_runtime(GTK_WINDOW(window), GTK_BOX(content));
  if (!fixture.runtime) { std::filesystem::remove_all(fixture.transfer_directory); return 1; }
  terminal_runtime_set_output_callback(fixture.runtime,
      [&fixture](std::span<const unsigned char>, std::span<const unsigned char> cooked) {
        fixture.normal_output.append(reinterpret_cast<const char *>(cooked.data()), cooked.size());
      });
  terminal_runtime_set_changed_callback(fixture.runtime, [](void *context) {
    static_cast<Fixture *>(context)->changed.trigger();
  }, &fixture);
  gtk_widget_show_all(window);
  std::exception_ptr failure;
  auto completion = run(&fixture, &failure);
  gtk_main();
  destroy_terminal_runtime(fixture.runtime);
  gtk_widget_destroy(window);
  std::filesystem::remove_all(fixture.transfer_directory);
  if (failure) {
    try { std::rethrow_exception(failure); }
    catch (const std::exception &error) { g_printerr("%s\n", error.what()); }
    return 1;
  }
  return 0;
}
