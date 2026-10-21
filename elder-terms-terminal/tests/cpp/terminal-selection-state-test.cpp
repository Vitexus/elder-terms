#include "../../src/terminal-presentation.h"
#include "../../src/terminal-runtime.h"

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>

using namespace elder_terms;

struct Fixture {
  MainWindow *window;
  TerminalPresentation *presentation;
  cardio::primitives::conditional changed;
  cardio::primitives::conditional received;
  std::string outgoing;
  bool accept_commit = false;
  std::string committed;
};

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static boxrig_ui_view *view(Fixture *fixture) {
  return boxrig_gtk3_terminal_widget_view(terminal_runtime_widget(fixture->window->runtime));
}

static void key(Fixture *fixture, guint symbol, GdkEventType type, guint modifiers) {
  auto *widget = GTK_WIDGET(terminal_runtime_widget(fixture->window->runtime));
  GdkEventKey event{};
  event.type = type;
  event.window = gtk_widget_get_window(widget);
  event.send_event = TRUE;
  event.time = GDK_CURRENT_TIME;
  event.keyval = symbol;
  event.hardware_keycode = symbol == GDK_KEY_Escape ? 9 : symbol == GDK_KEY_Return ? 36 : 38;
  event.state = modifiers;
  gboolean handled = FALSE;
  g_signal_emit_by_name(widget, type == GDK_KEY_PRESS ? "key-press-event" : "key-release-event", &event, &handled);
  expect(handled, "native terminal accepts the key event");
}

static void mouse(Fixture *fixture, GdkEventType type, double column, double row, guint modifiers) {
  auto *widget = terminal_runtime_widget(fixture->window->runtime);
  boxrig_font_effective_metrics metrics{};
  expect(boxrig_gtk3_terminal_widget_get_effective_metrics(widget, &metrics) == BOXRIG_FONT_CATALOG_OK, "mouse metrics");
  const auto scale = gtk_widget_get_scale_factor(GTK_WIDGET(widget));
  const auto x = column * metrics.cell_width / scale;
  const auto y = row * metrics.cell_height / scale;
  gboolean handled = FALSE;
  if (type == GDK_MOTION_NOTIFY) {
    GdkEventMotion event{};
    event.type = type;
    event.window = gtk_widget_get_window(GTK_WIDGET(widget));
    event.send_event = TRUE;
    event.x = x;
    event.y = y;
    event.state = modifiers;
    g_signal_emit_by_name(widget, "motion-notify-event", &event, &handled);
  } else {
    GdkEventButton event{};
    event.type = type;
    event.window = gtk_widget_get_window(GTK_WIDGET(widget));
    event.send_event = TRUE;
    event.x = x;
    event.y = y;
    event.button = 1;
    event.state = modifiers;
    g_signal_emit_by_name(widget, type == GDK_BUTTON_RELEASE ? "button-release-event" : "button-press-event", &event, &handled);
  }
  expect(handled, "native terminal accepts the pointer event");
}

static bool selected(Fixture *fixture) {
  boxrig_ui_selection selection{};
  return boxrig_ui_view_get_selection(view(fixture), &selection);
}

static void select(Fixture *fixture, boxrig_ui_selection_shape shape, bool empty) {
  auto *widget = terminal_runtime_widget(fixture->window->runtime);
  boxrig_font_effective_metrics metrics{};
  expect(boxrig_gtk3_terminal_widget_get_effective_metrics(widget, &metrics) == BOXRIG_FONT_CATALOG_OK, "selection metrics");
  boxrig_ui_hit_test_result hit{};
  expect(boxrig_ui_view_hit_test(view(fixture), BOXRIG_UI_FIXED_ONE,
      metrics.cell_height / 2 * BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK, "selection start");
  boxrig_ui_selection_options options{};
  options.kind = BOXRIG_UI_SELECTION_CHARACTER;
  options.shape = shape;
  expect(boxrig_ui_view_begin_selection_with_options(view(fixture), &options, &hit) == BOXRIG_UI_OK, "begin retained selection");
  if (!empty) expect(boxrig_ui_view_hit_test(view(fixture), metrics.cell_width * 8 * BOXRIG_UI_FIXED_ONE,
      metrics.cell_height / 2 * BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK, "selection end");
  expect(boxrig_ui_view_update_selection(view(fixture), &hit) == BOXRIG_UI_OK && selected(fixture), "retain selection anchor");
  if (empty) {
    boxrig_ui_selection selection{};
    expect(boxrig_ui_view_get_selection(view(fixture), &selection) && selection.row_count == 0,
        "zero-width rectangle retains its empty anchor");
  }
}

static cardio::promise<std::string> clipboard(GdkAtom selection) {
  struct Request {
    cardio::primitives::conditional ready;
    bool complete = false;
    std::string text;
  } request;
  gtk_clipboard_request_text(gtk_clipboard_get(selection), +[](GtkClipboard *, const char *text, gpointer context) {
    auto *request = static_cast<Request *>(context);
    request->text = text ? text : "";
    request->complete = true;
    request->ready.trigger();
  }, &request);
  while (!request.complete) co_await request.ready.wait();
  co_return request.text;
}

static cardio::promise<void> barrier(Fixture *fixture, bool kitty) {
  // A non-text key avoids the IME's committed-text path in Kitty mode.
  key(fixture, GDK_KEY_Return, GDK_KEY_PRESS, 0);
  key(fixture, GDK_KEY_Return, GDK_KEY_RELEASE, 0);
  const std::string tail = kitty ? "\033[13;1:3u" : "\r";
  while (!fixture->outgoing.ends_with(tail) || !terminal_runtime_is_settled(fixture->window->runtime))
    co_await fixture->changed.wait();
}

static cardio::promise<void> click_scenarios(Fixture *fixture) {
  auto *runtime = fixture->window->runtime;
  expect(terminal_runtime_feed(runtime, "\033[=0u\033[2J\033[Halpha beta gamma\r\nsecond row"), "prepare click text");
  while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
  gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), "click primary", -1);
  select(fixture, BOXRIG_UI_SELECTION_LINEAR, false);
  mouse(fixture, GDK_BUTTON_PRESS, 0.1, 0.5, 0);
  co_await barrier(fixture, false);
  expect(!selected(fixture), "single press clears the old selection without selecting one character");
  mouse(fixture, GDK_BUTTON_RELEASE, 0.1, 0.5, GDK_BUTTON1_MASK);
  co_await barrier(fixture, false);
  expect(!selected(fixture) && co_await clipboard(GDK_SELECTION_PRIMARY) == "click primary",
      "single click preserves PRIMARY and has no selected character");

  auto *widget = GTK_WIDGET(terminal_runtime_widget(runtime));
  boxrig_font_effective_metrics metrics{};
  expect(boxrig_gtk3_terminal_widget_get_effective_metrics(terminal_runtime_widget(runtime), &metrics) == BOXRIG_FONT_CATALOG_OK,
      "drag threshold metrics");
  int threshold = 0;
  g_object_get(gtk_widget_get_settings(widget), "gtk-dnd-drag-threshold", &threshold, nullptr);
  const auto columns_per_dip = static_cast<double>(gtk_widget_get_scale_factor(widget)) / metrics.cell_width;
  mouse(fixture, GDK_BUTTON_PRESS, 1.25 * columns_per_dip, 0.5, 0);
  mouse(fixture, GDK_MOTION_NOTIFY, (threshold + 1.25) * columns_per_dip, 0.5, GDK_BUTTON1_MASK);
  co_await barrier(fixture, false);
  expect(!selected(fixture), "motion at the GTK threshold remains a pending click at either display scale");
  mouse(fixture, GDK_MOTION_NOTIFY, (threshold + 2.25) * columns_per_dip, 0.5, GDK_BUTTON1_MASK);
  co_await barrier(fixture, false);
  expect(selected(fixture), "motion beyond the GTK threshold starts a selection");
  mouse(fixture, GDK_BUTTON_RELEASE, 8.9, 0.5, GDK_BUTTON1_MASK);
  co_await barrier(fixture, false);
  expect((co_await clipboard(GDK_SELECTION_PRIMARY)).starts_with("alpha"), "drag starts at the original press");

  for (const auto type : {GDK_2BUTTON_PRESS, GDK_3BUTTON_PRESS}) {
    mouse(fixture, type, 1.1, 0.5, 0);
    co_await barrier(fixture, false);
    boxrig_ui_selection selection{};
    expect(boxrig_ui_view_get_selection(view(fixture), &selection) && selection.kind ==
        (type == GDK_2BUTTON_PRESS ? BOXRIG_UI_SELECTION_WORD : BOXRIG_UI_SELECTION_LINE),
        "multiple clicks immediately select a word or line");
    mouse(fixture, GDK_BUTTON_RELEASE, 1.1, 0.5, GDK_BUTTON1_MASK);
    co_await barrier(fixture, false);
    const auto copied = co_await clipboard(GDK_SELECTION_PRIMARY);
    expect(type == GDK_2BUTTON_PRESS ? copied == "alpha" : copied.starts_with("alpha beta gamma"),
        "word and line selection copy their text");
    mouse(fixture, type, 1.1, 0.5, GDK_CONTROL_MASK);
    mouse(fixture, GDK_MOTION_NOTIFY, 8.9, 1.5, GDK_BUTTON1_MASK);
    mouse(fixture, GDK_BUTTON_RELEASE, 8.9, 1.5, GDK_BUTTON1_MASK);
    co_await barrier(fixture, false);
    expect(boxrig_ui_view_get_selection(view(fixture), &selection) &&
        selection.kind == BOXRIG_UI_SELECTION_CHARACTER && selection.shape == BOXRIG_UI_SELECTION_RECTANGULAR,
        "Ctrl multiple clicks use character rectangles even after Ctrl is released");
  }

  for (const auto modifier : {GDK_SHIFT_MASK, GDK_CONTROL_MASK}) {
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), "cancel pending", -1);
    mouse(fixture, GDK_BUTTON_PRESS, 0.1, 0.5, modifier);
    key(fixture, GDK_KEY_Escape, GDK_KEY_PRESS, 0);
    key(fixture, GDK_KEY_Escape, GDK_KEY_RELEASE, 0);
    mouse(fixture, GDK_MOTION_NOTIFY, 8.9, 0.5, GDK_BUTTON1_MASK);
    mouse(fixture, GDK_BUTTON_RELEASE, 8.9, 0.5, GDK_BUTTON1_MASK);
    fixture->outgoing.clear();
    co_await barrier(fixture, false);
    expect(!selected(fixture) && fixture->outgoing == "\r" &&
        co_await clipboard(GDK_SELECTION_PRIMARY) == "cancel pending", "Escape cancels a pending click without side effects");
  }

  // A retained press must never acquire a new generation after terminal output.
  mouse(fixture, GDK_BUTTON_PRESS, 0.1, 0.5, GDK_CONTROL_MASK);
  co_await barrier(fixture, false);
  expect(terminal_runtime_feed(runtime, "\033[Hchanged text"), "replace text under a pending click");
  while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
  mouse(fixture, GDK_MOTION_NOTIFY, 8.9, 0.5, GDK_BUTTON1_MASK);
  mouse(fixture, GDK_BUTTON_RELEASE, 8.9, 0.5, GDK_BUTTON1_MASK);
  co_await barrier(fixture, false);
  expect(!selected(fixture) && co_await clipboard(GDK_SELECTION_PRIMARY) == "cancel pending",
      "a stale pending press aborts without selecting replacement text");

  std::vector<TerminalHyperlinkResolution> actions;
  terminal_runtime_set_hyperlink_rules(runtime, default_hyperlink_action_rules(),
      [&actions](TerminalHyperlinkResolution action) { actions.push_back(std::move(action)); });
  for (const bool osc8 : {false, true}) {
    const std::string text = osc8 ? "\033]8;;https://link.test/\033\\Open link\033]8;;\033\\" : "https://link.test/";
    expect(terminal_runtime_feed(runtime, "\033[2J\033[H" + text + "\r\nsecond row\033[?1002h\033[?1006h"),
        "prepare links with mouse reporting");
    while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
    for (const auto modifier : {guint(GDK_CONTROL_MASK), guint(GDK_CONTROL_MASK | GDK_SHIFT_MASK)}) {
      const auto previous = actions.size();
      mouse(fixture, GDK_BUTTON_PRESS, 2.1, 0.5, modifier);
      co_await barrier(fixture, false);
      expect(actions.size() == previous && !selected(fixture), "link activation waits for release");
      mouse(fixture, GDK_MOTION_NOTIFY, 8.9, 1.5, GDK_BUTTON1_MASK);
      mouse(fixture, GDK_BUTTON_RELEASE, 8.9, 1.5, GDK_BUTTON1_MASK);
      fixture->outgoing.clear();
      co_await barrier(fixture, false);
      boxrig_ui_selection selection{};
      expect(actions.size() == previous && boxrig_ui_view_get_selection(view(fixture), &selection) &&
          selection.shape == BOXRIG_UI_SELECTION_RECTANGULAR && fixture->outgoing == "\r",
          "Ctrl drag over regex and OSC 8 links selects a rectangle without launching or terminal mouse reports");
    }
    const auto previous = actions.size();
    mouse(fixture, GDK_BUTTON_PRESS, 2.1, 0.5, GDK_CONTROL_MASK);
    mouse(fixture, GDK_BUTTON_RELEASE, 2.1, 0.5, GDK_BUTTON1_MASK | GDK_CONTROL_MASK);
    co_await barrier(fixture, false);
    expect(actions.size() == previous + 1 && actions.back().action &&
        actions.back().action->arguments == std::vector<std::string>{"https://link.test/"},
        "Ctrl click launches the same valid link once on release");
    mouse(fixture, GDK_BUTTON_PRESS, 2.1, 0.5, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    mouse(fixture, GDK_BUTTON_RELEASE, 2.1, 0.5, GDK_BUTTON1_MASK | GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    co_await barrier(fixture, false);
    expect(actions.size() == previous + 1, "Ctrl Shift click never launches a link");
    mouse(fixture, GDK_BUTTON_PRESS, 2.1, 0.5, GDK_CONTROL_MASK);
    co_await barrier(fixture, false);
    expect(terminal_runtime_feed(runtime, "\033[Hremoved link         "), "invalidate a pending link");
    while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
    mouse(fixture, GDK_BUTTON_RELEASE, 2.1, 0.5, GDK_BUTTON1_MASK);
    co_await barrier(fixture, false);
    expect(actions.size() == previous + 1, "an invalidated link does not launch on release");
    const std::string adjacent = osc8 ?
        "\033]8;id=first;https://a.test/\033\\left\033]8;;\033\\ "
        "\033]8;id=second;https://a.test/\033\\right\033]8;;\033\\" :
        "https://a.test/ https://a.test/";
    expect(terminal_runtime_feed(runtime, "\033[2J\033[H" + adjacent), "prepare distinct links with identical actions");
    while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
    const auto boundary = osc8 ? 4.0 : 15.0;
    mouse(fixture, GDK_BUTTON_PRESS, boundary - 0.1, 0.5, GDK_CONTROL_MASK);
    mouse(fixture, GDK_BUTTON_RELEASE, boundary + 1.1, 0.5, GDK_BUTTON1_MASK | GDK_CONTROL_MASK);
    co_await barrier(fixture, false);
    expect(actions.size() == previous + 1, "release on a different link with the same URL does not activate it");
  }
  terminal_runtime_set_hyperlink_rules(runtime, {}, {});
  fixture->outgoing.clear();
  mouse(fixture, GDK_BUTTON_PRESS, 0.1, 0.5, 0);
  mouse(fixture, GDK_MOTION_NOTIFY, 8.9, 0.5, GDK_BUTTON1_MASK);
  mouse(fixture, GDK_BUTTON_RELEASE, 8.9, 0.5, GDK_BUTTON1_MASK);
  co_await barrier(fixture, false);
  expect(fixture->outgoing == "\033[<0;1;1M\033[<32;9;1M\033[<0;9;1m\r" && !selected(fixture),
      "unmodified mouse reports reach the peer as one complete sequence");
  expect(terminal_runtime_feed(runtime, "\033[?1002l\033[?1006l"), "restore ordinary mouse input");
}

static cardio::promise<void> scenarios(Fixture *fixture) {
  auto *runtime = fixture->window->runtime;
  TerminalTransportRequest request;
  request.open = [fixture](std::size_t, std::size_t, cardio::cancellation) -> cardio::promise<TerminalTransport> {
    co_return TerminalTransport{
      [fixture](std::span<unsigned char>, cardio::cancellation stop) -> cardio::promise<std::size_t> {
        co_await fixture->received.wait(stop);
        co_return 0;
      },
      [fixture](std::span<const unsigned char> bytes, cardio::cancellation stop) -> cardio::promise<void> {
        stop.throw_if_cancellation_requested();
        fixture->outgoing.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        fixture->changed.trigger();
        co_return;
      },
      [](std::size_t, std::size_t, cardio::cancellation) -> cardio::promise<void> { co_return; },
      []() -> cardio::promise<void> { co_return; }};
  };
  expect(terminal_runtime_connect_transport(runtime, std::move(request)), "connect observable selection peer");
  set_main_window_connection_phase(fixture->window, TerminalSessionConnectionPhase::connected);
  while (!terminal_runtime_is_settled(runtime) ||
      terminal_runtime_connection_state(runtime).phase != TerminalConnectionPhase::connected)
    co_await fixture->changed.wait();
  gtk_widget_grab_focus(GTK_WIDGET(terminal_runtime_widget(runtime)));
  GdkEventFocus focus{};
  focus.type = GDK_FOCUS_CHANGE;
  focus.window = gtk_widget_get_window(GTK_WIDGET(terminal_runtime_widget(runtime)));
  focus.in = TRUE;
  gboolean handled = FALSE;
  g_signal_emit_by_name(terminal_runtime_widget(runtime), "focus-in-event", &focus, &handled);
  gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), "retained primary", -1);
  gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), "retained clipboard", -1);

  for (const bool kitty : {false, true}) {
    expect(terminal_runtime_feed(runtime, kitty ? "\033[>11u" : "\033[=0u"), "set keyboard reporting");
    const std::string marker = kitty ? "\033[13u\033[13;1:3u" : "\r";
    for (const auto scenario : {0, 1, 2, 3}) {
      expect(terminal_runtime_feed(runtime, "\033[2J\033[HSELECTABLE TEXT"), "prepare selectable text");
      while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
      select(fixture, scenario == 0 ? BOXRIG_UI_SELECTION_LINEAR : BOXRIG_UI_SELECTION_RECTANGULAR, scenario == 2);
      if (scenario == 3) {
        expect(terminal_runtime_feed(runtime, "\033[Hchanged"), "invalidate selected text");
        while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
        boxrig_ui_selection selection{};
        expect(boxrig_ui_view_get_selection(view(fixture), &selection) && selection.stale, "retain stale rectangle");
      }
      fixture->outgoing.clear();
      key(fixture, GDK_KEY_Escape, GDK_KEY_PRESS, GDK_LOCK_MASK);
      key(fixture, GDK_KEY_Escape, GDK_KEY_PRESS, GDK_LOCK_MASK);
      key(fixture, GDK_KEY_Escape, GDK_KEY_RELEASE, GDK_LOCK_MASK);
      co_await barrier(fixture, kitty);
      expect(!selected(fixture), "Escape clears normal, rectangular, empty and stale selections despite Caps Lock");
      if (fixture->outgoing != marker) {
        g_printerr("keyboard kitty=%d scenario=%d received:", kitty, scenario);
        for (const unsigned char byte : fixture->outgoing) g_printerr(" %02x", byte);
        g_printerr("\n");
      }
      expect(fixture->outgoing == marker, "cancelled Escape repeat and release never reach the peer");
      expect(co_await clipboard(GDK_SELECTION_PRIMARY) == "retained primary", "Escape preserves PRIMARY");
      expect(co_await clipboard(GDK_SELECTION_CLIPBOARD) == "retained clipboard", "Escape preserves CLIPBOARD");
    }
    fixture->outgoing.clear();
    key(fixture, GDK_KEY_Escape, GDK_KEY_PRESS, 0);
    key(fixture, GDK_KEY_Escape, GDK_KEY_RELEASE, 0);
    co_await barrier(fixture, kitty);
    expect(fixture->outgoing == (kitty ? "\033[27u\033[27;1:3u" : "\033") + marker,
        "Escape without a selection retains the terminal protocol");
    for (const auto modifier : {GDK_SHIFT_MASK, GDK_CONTROL_MASK, GDK_MOD1_MASK, GDK_SUPER_MASK}) {
      select(fixture, BOXRIG_UI_SELECTION_LINEAR, false);
      fixture->outgoing.clear();
      key(fixture, GDK_KEY_Escape, GDK_KEY_PRESS, modifier);
      key(fixture, GDK_KEY_Escape, GDK_KEY_RELEASE, modifier);
      co_await barrier(fixture, kitty);
      expect(selected(fixture) && fixture->outgoing.size() > marker.size(), "modified Escape remains terminal input");
    }
    expect(boxrig_ui_view_clear_selection(view(fixture)) == BOXRIG_UI_OK, "clear modified-key selection");
    expect(terminal_runtime_feed(runtime, "\033[?1002h\033[?1006h"), "enable terminal mouse reports");
    while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
    for (const auto modifier : {GDK_SHIFT_MASK, GDK_CONTROL_MASK}) {
      fixture->outgoing.clear();
      mouse(fixture, GDK_BUTTON_PRESS, 0.1, 0.5, modifier);
      mouse(fixture, GDK_MOTION_NOTIFY, 8.9, 0.5, modifier | GDK_BUTTON1_MASK);
      co_await barrier(fixture, kitty);
      expect(selected(fixture), "a held override drag has a selection");
      fixture->outgoing.clear();
      key(fixture, GDK_KEY_Escape, GDK_KEY_PRESS, 0);
      key(fixture, GDK_KEY_Escape, GDK_KEY_RELEASE, 0);
      mouse(fixture, GDK_MOTION_NOTIFY, 12.9, 1.5, GDK_BUTTON1_MASK);
      mouse(fixture, GDK_BUTTON_RELEASE, 12.9, 1.5, GDK_BUTTON1_MASK);
      co_await barrier(fixture, kitty);
      expect(!selected(fixture) && fixture->outgoing == marker,
          "cancelled held drag never resumes selection or leaks mouse/key events");
      expect(co_await clipboard(GDK_SELECTION_PRIMARY) == "retained primary", "cancelled release does not copy");
    }
    expect(terminal_runtime_feed(runtime, "\033[?1002l\033[?1006l"), "restore ordinary mouse input");
  }
  co_await click_scenarios(fixture);
  while (!terminal_runtime_is_settled(runtime)) co_await fixture->changed.wait();
  select(fixture, BOXRIG_UI_SELECTION_RECTANGULAR, false);
  auto *composition = boxrig_gtk3_terminal_widget_composition(terminal_runtime_widget(runtime));
  expect(boxrig_composition_set_commit_sink(composition, +[](const char *text, std::size_t size, void *context) {
    auto *fixture = static_cast<Fixture *>(context);
    if (!fixture->accept_commit) return BOXRIG_STATUS_WOULD_BLOCK;
    fixture->committed.append(text, size);
    return BOXRIG_STATUS_OK;
  }, fixture) == BOXRIG_COMPOSITION_OK, "install a bounded commit destination");
  boxrig_composition_event commit{};
  commit.type = BOXRIG_COMPOSITION_COMMIT;
  commit.text = "確定";
  commit.text_length = std::char_traits<char>::length(commit.text);
  expect(boxrig_composition_apply_event(composition, &commit) == BOXRIG_COMPOSITION_WOULD_BLOCK,
      "composition retains an accepted commit while the destination is full");
  boxrig_key_event escape{};
  escape.key = BOXRIG_KEY_ESCAPE;
  escape.action = BOXRIG_KEY_PRESS;
  expect(terminal_runtime_route_selection_key(runtime, escape) == BOXRIG_INPUT_KEY_ROUTE_TERMINAL && selected(fixture),
      "pending commit keeps Escape in the IME path and retains selection");
  fixture->accept_commit = true;
  expect(boxrig_composition_retry_commit(composition) == BOXRIG_COMPOSITION_OK && fixture->committed == "確定",
      "selection routing does not discard or duplicate the accepted commit");
  expect(terminal_runtime_route_selection_key(runtime, escape) == BOXRIG_INPUT_KEY_ROUTE_APPLICATION && !selected(fixture),
      "selection cancellation resumes after commit delivery");
}

static cardio::promise<void> run(Fixture *fixture, std::exception_ptr *failure) {
  try { co_await scenarios(fixture); }
  catch (...) { *failure = std::current_exception(); }
  co_await terminal_runtime_disconnect(fixture->window->runtime);
  gtk_main_quit();
}

extern "C" int __wrap_main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  auto window = load_main_window();
  if (!window) return 1;
  Fixture fixture{&*window, create_terminal_presentation(&*window, {}, {}, {}), {}, {}, {}, false, {}};
  terminal_runtime_set_host_callbacks(window->runtime, {
      [&fixture](BoxrigGtk3TerminalWidget *) {
        attach_main_window_terminal(fixture.window);
        attach_terminal_presentation(fixture.presentation);
      }, {}, {}, [&fixture](const boxrig_mouse_event *event, const boxrig_ui_hit_test_result *) {
        const auto route = route_main_window_terminal_mouse(fixture.window, *event);
        return route == BOXRIG_INPUT_MOUSE_ROUTE_TERMINAL ?
            route_terminal_presentation_mouse(fixture.presentation, *event) : route;
      }});
  terminal_runtime_set_changed_callback(window->runtime, [](void *context) {
    static_cast<Fixture *>(context)->changed.trigger();
  }, &fixture);
  gtk_widget_show(window->window);
  std::exception_ptr failure;
  auto completion = run(&fixture, &failure);
  gtk_main();
  terminal_runtime_set_changed_callback(window->runtime, nullptr, nullptr);
  terminal_runtime_set_host_callbacks(window->runtime, {});
  destroy_terminal_presentation(fixture.presentation);
  destroy_terminal_runtime(window->runtime);
  window->runtime = nullptr;
  window->terminal = nullptr;
  gtk_widget_destroy(window->window);
  release_main_window(&*window);
  if (failure) {
    try { std::rethrow_exception(failure); }
    catch (const std::exception &error) { g_printerr("%s\n", error.what()); }
    return 1;
  }
  std::puts("PASS selection cancellation, retained clipboards and complete native input sequences");
  return 0;
}
