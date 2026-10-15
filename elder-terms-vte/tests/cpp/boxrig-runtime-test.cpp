#include "../../src/terminal-runtime.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <system_error>

using namespace elder_terms;

struct Fixture {
  TerminalRuntime *runtime = nullptr;
  cardio::primitives::conditional changed;
  cardio::primitives::conditional received;
  cardio::primitives::conditional resize_ready;
  std::string incoming;
  std::string outgoing;
  bool resize_pending = false;
  bool fail_resize = false;
  bool hold_resize = true;
  bool closed = false;
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
  while (fixture->incoming.empty()) co_await fixture->received.wait(cancellation);
  const auto count = std::min(bytes.size(), fixture->incoming.size());
  std::memcpy(bytes.data(), fixture->incoming.data(), count);
  fixture->incoming.erase(0, count);
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
    co_return TerminalTransport{
        [fixture](std::span<unsigned char> bytes, cardio::cancellation stop) { return read_peer(fixture, bytes, stop); },
        [fixture](std::span<const unsigned char> bytes, cardio::cancellation stop) -> cardio::promise<void> {
          stop.throw_if_cancellation_requested();
          fixture->outgoing.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
          fixture->changed.trigger();
          co_return;
        },
        [fixture](std::size_t rows, std::size_t columns, cardio::cancellation stop) { return resize_peer(fixture, rows, columns, stop); },
        [fixture]() -> cardio::promise<void> { fixture->closed = true; co_return; }};
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
  expect(terminal_runtime_set_preferred_grid(fixture->runtime, 30, 100) == BOXRIG_UI_OK, "request first grid");
  while (!fixture->resize_pending) co_await fixture->changed.wait();
  expect(fixture->requested_rows == 30 && fixture->requested_columns == 100, "peer receives requested grid");
  boxrig_ui_layout_result layout{};
  fixture->incoming = "\033[999;999H\033[6n\033[2;1H";
  fixture->received.trigger();
  while (fixture->outgoing.find('R') == std::string::npos) co_await fixture->changed.wait();
  expect(fixture->outgoing == "\033[24;80R", "terminal replies use the confirmed grid while peer resize is pending");
  expect(terminal_runtime_set_preferred_grid(fixture->runtime, 32, 110) == BOXRIG_UI_OK, "retain newer allocation");
  fixture->resize_ready.trigger();
  while (fixture->requested_columns != 110) co_await fixture->changed.wait();
  fixture->resize_ready.trigger();
  while (!terminal_runtime_is_settled(fixture->runtime)) co_await fixture->changed.wait();
  expect(boxrig_gtk3_terminal_widget_get_layout(widget, &layout) == BOXRIG_UI_OK && layout.columns == 110,
      "later grid completes after its own peer request");

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
  fixture.runtime = create_terminal_runtime(GTK_WINDOW(window), GTK_BOX(content));
  if (!fixture.runtime) return 1;
  terminal_runtime_set_changed_callback(fixture.runtime, [](void *context) {
    static_cast<Fixture *>(context)->changed.trigger();
  }, &fixture);
  gtk_widget_show_all(window);
  std::exception_ptr failure;
  auto completion = run(&fixture, &failure);
  gtk_main();
  destroy_terminal_runtime(fixture.runtime);
  gtk_widget_destroy(window);
  if (failure) {
    try { std::rethrow_exception(failure); }
    catch (const std::exception &error) { g_printerr("%s\n", error.what()); }
    return 1;
  }
  return 0;
}
