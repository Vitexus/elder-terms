#include "../../src/terminal-connection.h"

#include <glib.h>
#include <csignal>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

using namespace elder_terms;

struct Test {
  boxrig_terminal *core = nullptr;
  TerminalConnection *connection = nullptr;
  cardio::primitives::conditional changed;
  guint dispatch_source = 0;
  bool paused = false;
  std::string raw;
  std::string cooked;
  std::size_t titles = 0;
};

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static void wake(Test *test);

static gboolean dispatch(gpointer context) {
  auto *test = static_cast<Test *>(context);
  test->dispatch_source = 0;
  const auto progress = terminal_connection_dispatch(test->connection);
  boxrig_host_item item{};
  bool consumed = false;
  while (boxrig_terminal_host_item_peek(test->core, &item)) {
    if (item.type == BOXRIG_HOST_ITEM_TITLE) ++test->titles;
    const auto request = item.request_id;
    boxrig_terminal_host_item_consume(test->core);
    if (request) boxrig_terminal_complete_host_request(test->core, request, BOXRIG_HOST_REQUEST_UNAVAILABLE, nullptr, 0);
    consumed = true;
  }
  if (progress.dispatch_pending || consumed) wake(test);
  test->changed.trigger();
  return G_SOURCE_REMOVE;
}

static void wake(Test *test) {
  test->changed.trigger();
  if (!test->paused && !test->dispatch_source)
    test->dispatch_source = g_idle_add_full(G_PRIORITY_DEFAULT, dispatch, test, nullptr);
}

static std::string contents(Test *test) {
  const boxrig_text_snapshot_options options{BOXRIG_TEXT_SCREEN_PRIMARY, nullptr, 65536, 1024};
  boxrig_text_snapshot *snapshot = nullptr;
  expect(boxrig_terminal_create_text_snapshot(test->core, &options, &snapshot) == BOXRIG_STATUS_OK, "text snapshot");
  size_t length = 0;
  const auto *bytes = boxrig_text_snapshot_data(snapshot, &length);
  const std::string text(reinterpret_cast<const char *>(bytes), length);
  boxrig_text_snapshot_destroy(snapshot);
  return text;
}

static TerminalLocalCommand command(std::string script) {
  return {{"/bin/sh", "-c", std::move(script)}, "xterm-256color", 17, 3};
}

static cardio::promise<void> scenarios(Test *test) {
  test->paused = true;
  expect(terminal_connection_start_local(test->connection, command(
      "stty raw -echo; i=0; while [ \"$i\" -lt 200 ]; do printf '\\033]2;x\\007\\033[6n'; i=$((i+1)); done; "
      "expected=$(i=0; while [ \"$i\" -lt 200 ]; do printf '\\033[1;1R'; i=$((i+1)); done); "
      "actual=$(dd bs=1 count=1200 iflag=fullblock 2>/dev/null); "
      "if [ \"$actual\" = \"$expected\" ]; then printf '\\r\\nSTRESS_OK\\r\\n\\342\\202'; else printf BAD_REPLY; exit 3; fi")), "start stress peer");
  while (terminal_connection_state(test->connection).receive_peak != 17) co_await test->changed.wait();
  expect(terminal_connection_state(test->connection).receive_peak == 17, "receive queue reaches its exact bound");
  test->paused = false;
  wake(test);
  co_await terminal_connection_wait_stopped(test->connection);
  auto state = terminal_connection_state(test->connection);
  expect(state.exit_code == 0 && state.reaped && state.operation_error == 0, "stress peer completes and is reaped");
  expect(state.receive_peak <= 17 && state.send_peak > 0 && state.send_peak <= 17, "both queues remain bounded");
  expect(test->raw.ends_with("\xe2\x82"), "raw output preserves the incomplete UTF-8 bytes");
  expect(test->cooked == test->raw.substr(0, test->raw.size() - 2) + "�",
      "cooked output includes the decoder's one EOF replacement");
  expect(test->titles == 200, "host backpressure does not duplicate or lose title events");
  expect(contents(test).find("STRESS_OK\n�") != std::string::npos, "EOF drains UTF-8 tail after all replies");
  expect(::kill(state.child, 0) == -1 && errno == ESRCH, "no zombie stress child");

  const auto previous_size = test->raw.size();
  expect(terminal_connection_start_local(test->connection, command("stty -echo; printf '\\342'; read value")), "start incomplete UTF-8 peer");
  while (test->raw.size() == previous_size) co_await test->changed.wait();
  terminal_connection_dispatch(test->connection);
  terminal_connection_stop(test->connection);
  co_await terminal_connection_wait_stopped(test->connection);
  expect(contents(test).find("��") != std::string::npos, "disconnect finalizes this generation's partial UTF-8");

  expect(terminal_connection_start_local(test->connection, command("printf '\\202\\254RECONNECTED'")), "reconnect after incomplete input");
  co_await terminal_connection_wait_stopped(test->connection);
  expect(contents(test).find("€") == std::string::npos, "old and new generation bytes never form one character");
  expect(contents(test).find("RECONNECTED") != std::string::npos, "core and display survive reconnection");

  test->paused = true;
  expect(terminal_connection_start_local(test->connection,
      command("stty -echo; printf 'CANCEL_QUEUE_TAIL'; read value")), "start queued cancellation peer");
  while (terminal_connection_state(test->connection).receive_peak != 17) co_await test->changed.wait();
  terminal_connection_stop(test->connection);
  test->paused = false;
  wake(test);
  co_await terminal_connection_wait_stopped(test->connection);
  expect(contents(test).find("CANCEL_QUEUE_TAIL") != std::string::npos, "cancellation preserves bytes already received");

  auto encoded = command(
      "stty raw -echo; printf 'ENCODE_READY'; "
      "expected=$(printf '\\223\\372\\226\\173\\214\\352\\223\\372\\226\\173\\214\\352'); "
      "actual=$(dd bs=1 count=12 iflag=fullblock 2>/dev/null); "
      "if [ \"$actual\" != \"$expected\" ]; then printf BAD_ENCODING; exit 3; fi; "
      "printf '\\r\\n\\223\\372\\226\\173\\214\\352\\202'");
  encoded.encoding = "CP932";
  encoded.queue_capacity = 1;
  encoded.io_chunk_size = 1;
  const auto encoded_start = test->cooked.size();
  expect(terminal_connection_start_local(test->connection, std::move(encoded)), "start encoded stream");
  while (test->cooked.find("ENCODE_READY", encoded_start) == std::string::npos) co_await test->changed.wait();
  const std::string text = "日本語日本語";
  expect(boxrig_terminal_send_text(test->core, text.data(), text.size()) == BOXRIG_STATUS_OK, "queue UTF-8 text");
  wake(test);
  co_await terminal_connection_wait_stopped(test->connection);
  state = terminal_connection_state(test->connection);
  expect(state.exit_code == 0 && state.reaped, "CP932 peer receives each encoded byte exactly once");
  expect(test->cooked.substr(encoded_start) == "ENCODE_READY\r\n日本語�", "split CP932 input and EOF are converted exactly once");
  expect(state.receive_peak == 1 && state.send_peak == 1, "converted characters span single-byte bounded queues");

  test->paused = true;
  auto cancelled_encoding = command("stty -echo; printf '\\223\\372'; read value");
  cancelled_encoding.encoding = "CP932";
  cancelled_encoding.queue_capacity = 1;
  cancelled_encoding.io_chunk_size = 1;
  const auto cancelled_start = test->cooked.size();
  expect(terminal_connection_start_local(test->connection, std::move(cancelled_encoding)), "start converted cancellation peer");
  while (terminal_connection_state(test->connection).receive_peak != 1) co_await test->changed.wait();
  terminal_connection_stop(test->connection);
  test->paused = false;
  wake(test);
  co_await terminal_connection_wait_stopped(test->connection);
  expect(test->cooked.substr(cancelled_start) == "日", "cancellation reports the converted chunk only once");
  expect(contents(test).find("日", contents(test).find("日本語�") + std::string("日本語�").size()) != std::string::npos,
      "cancellation preserves the converted remainder beyond queue capacity");

  auto invalid_encoding = command("printf SHOULD_NOT_SPAWN");
  invalid_encoding.encoding = "ELDER_TERMS_INVALID_ENCODING";
  expect(terminal_connection_start_local(test->connection, std::move(invalid_encoding)), "accept an observable conversion failure");
  co_await terminal_connection_wait_stopped(test->connection);
  state = terminal_connection_state(test->connection);
  expect(state.operation_error == EINVAL && state.child == 0, "invalid conversion fails before spawning a child");

  std::vector<unsigned char> live_input{0x93, 0xfa, 0x82};
  std::string live_output;
  cardio::primitives::conditional live_received;
  cardio::primitives::conditional release_write;
  bool live_eof = false;
  bool first_write = true;
  TerminalTransportRequest live;
  live.encoding = "CP932";
  live.queue_capacity = 1;
  live.io_chunk_size = 1;
  live.open = [&](std::size_t, std::size_t, cardio::cancellation) -> cardio::promise<TerminalTransport> {
    co_return TerminalTransport{
        [&](std::span<unsigned char> bytes, cardio::cancellation stop) -> cardio::promise<std::size_t> {
          while (live_input.empty() && !live_eof) co_await live_received.wait(stop);
          if (live_input.empty()) co_return 0;
          bytes[0] = live_input.front();
          live_input.erase(live_input.begin());
          co_return 1;
        },
        [&](std::span<const unsigned char> bytes, cardio::cancellation stop) -> cardio::promise<void> {
          live_output.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
          test->changed.trigger();
          if (std::exchange(first_write, false)) co_await release_write.wait(stop);
        },
        [](std::size_t, std::size_t, cardio::cancellation) -> cardio::promise<void> { co_return; },
        []() -> cardio::promise<void> { co_return; }};
  };
  const auto live_raw_start = test->raw.size();
  const auto live_cooked_start = test->cooked.size();
  expect(terminal_connection_start_transport(test->connection, std::move(live)), "start live encoding peer");
  while (test->raw.size() < live_raw_start + 3) co_await test->changed.wait();
  const auto live_generation = terminal_connection_state(test->connection).generation;
  expect(boxrig_terminal_send_text(test->core, "日", 3) == BOXRIG_STATUS_OK, "queue old-encoding text");
  wake(test);
  while (live_output.empty()) co_await test->changed.wait();
  auto exchange_encoding = terminal_connection_change_encoding(test->connection, "UTF-8");
  expect(terminal_connection_state(test->connection).encoding == "CP932", "encoding exchange waits for an in-flight write");
  release_write.trigger();
  expect(co_await exchange_encoding, "encoding exchange completes after the old write");
  expect(live_output == std::string("\x93\xfa", 2), "old output is completely encoded before exchange");
  expect(terminal_connection_state(test->connection).generation == live_generation, "encoding exchange preserves the connection generation");
  expect(!(co_await terminal_connection_change_encoding(test->connection, "ELDER_TERMS_INVALID_ENCODING")), "invalid live encoding is rejected");
  expect(terminal_connection_state(test->connection).encoding == "UTF-8", "rejected encoding leaves the old converters usable");
  expect(boxrig_terminal_send_text(test->core, "語", 3) == BOXRIG_STATUS_OK, "queue new-encoding text");
  wake(test);
  while (live_output.size() != 5) co_await test->changed.wait();
  expect(live_output == std::string("\x93\xfa", 2) + "語", "later output uses the new encoding exactly once");
  const std::string live_tail = "語";
  live_input.assign(live_tail.begin(), live_tail.end());
  live_eof = true;
  live_received.trigger();
  co_await terminal_connection_wait_stopped(test->connection);
  expect(test->cooked.substr(live_cooked_start) == "日�語", "live exchange flushes the old partial input before the new stream");

  const auto cancelled_exchange_start = test->cooked.size();
  bool cancelling_write_started = false;
  TerminalTransportRequest cancelling_exchange;
  cancelling_exchange.encoding = "ISO-2022-JP";
  cancelling_exchange.open = [&](std::size_t, std::size_t, cardio::cancellation) -> cardio::promise<TerminalTransport> {
    co_return TerminalTransport{
        [&](std::span<unsigned char>, cardio::cancellation stop) -> cardio::promise<std::size_t> {
          co_await live_received.wait(stop);
          co_return 0;
        },
        [&](std::span<const unsigned char>, cardio::cancellation stop) -> cardio::promise<void> {
          cancelling_write_started = true;
          test->changed.trigger();
          co_await release_write.wait(stop);
        },
        [](std::size_t, std::size_t, cardio::cancellation) -> cardio::promise<void> { co_return; },
        []() -> cardio::promise<void> { co_return; }};
  };
  expect(terminal_connection_start_transport(test->connection, std::move(cancelling_exchange)), "start cancelled encoding peer");
  while (terminal_connection_state(test->connection).phase != TerminalConnectionPhase::connected) co_await test->changed.wait();
  expect(boxrig_terminal_send_text(test->core, "日", 3) == BOXRIG_STATUS_OK, "queue text before cancelling encoding exchange");
  wake(test);
  while (!cancelling_write_started) co_await test->changed.wait();
  auto cancelled_exchange = terminal_connection_change_encoding(test->connection, "UTF-8");
  terminal_connection_stop(test->connection);
  expect(!(co_await cancelled_exchange), "disconnect cancels an encoding exchange waiting for output");
  co_await terminal_connection_wait_stopped(test->connection);
  expect(test->cooked.size() == cancelled_exchange_start, "cancelled empty input has no duplicate finalization");

  for (int attempt = 0; attempt < 4; ++attempt) {
    expect(terminal_connection_start_local(test->connection, command("trap '' HUP TERM; read value")), "start cancellable child");
    terminal_connection_stop(test->connection);
    expect(!terminal_connection_start_local(test->connection, command("exit 0")), "reconnect waits for recovery");
    co_await terminal_connection_wait_stopped(test->connection);
    state = terminal_connection_state(test->connection);
    expect(state.cancelled && state.reaped && state.operation_error == 0, "startup cancellation recovers the child");
    expect(::kill(state.child, 0) == -1 && errno == ESRCH, "cancelled child cannot remain a zombie");
  }

  expect(terminal_connection_start_local(test->connection, {}), "parent diagnostic is observable");
  co_await terminal_connection_wait_stopped(test->connection);
  state = terminal_connection_state(test->connection);
  expect(state.spawn.state == BOXRIG_POSIX_PTY_SPAWN_FAILED && state.spawn.stage == BOXRIG_POSIX_PTY_STAGE_ARGUMENTS &&
      state.spawn.error_number == EINVAL && state.child == 0 && !state.reaped, "parent preparation failure is separate from process exit");
}

static cardio::promise<void> run(Test *test, cardio::dispatcher_group_glib *group, std::exception_ptr *failure) {
  try { co_await scenarios(test); }
  catch (...) { *failure = std::current_exception(); }
  terminal_connection_stop(test->connection);
  co_await terminal_connection_wait_stopped(test->connection);
  group->shutdown();
}

int main() {
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib dispatcher(group);
  Test test;
  auto config = boxrig_terminal_config_default();
  config.output_capacity = 32;
  config.host_item_capacity = 2;
  if (boxrig_terminal_create(&config, &test.core) != BOXRIG_STATUS_OK) return 1;
  test.connection = create_terminal_connection(test.core, [&test] { wake(&test); },
      [&test](std::span<const unsigned char> raw, std::span<const unsigned char> cooked) {
        if (!raw.empty()) test.raw.append(reinterpret_cast<const char *>(raw.data()), raw.size());
        if (!cooked.empty()) test.cooked.append(reinterpret_cast<const char *>(cooked.data()), cooked.size());
        test.changed.trigger();
      });
  std::exception_ptr failure;
  auto completion = run(&test, &group, &failure);
  dispatcher.park();
  if (test.dispatch_source) g_source_remove(test.dispatch_source);
  destroy_terminal_connection(test.connection);
  boxrig_terminal_destroy(test.core);
  if (failure) {
    try { std::rethrow_exception(failure); }
    catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); }
    return 1;
  }
  std::puts("Bounded PTY I/O, EOF, reconnect, startup cancellation, and reaping passed.");
  return 0;
}
