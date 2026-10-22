// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <cardio.h>
#include <gtk/gtk.h>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <fcntl.h>
#include <unistd.h>

struct Fixture {
  unsigned int completed = 0;
  unsigned int observed = 0;
  bool observed_idle = false;
  int descriptors[2]{-1, -1};
  std::optional<cardio::promise<void>> work;
};

static gboolean close_window(gpointer) {
  auto *windows = gtk_window_list_toplevels();
  for (auto *item = windows; item; item = item->next) {
    if (gtk_widget_get_visible(GTK_WIDGET(item->data)))
      gtk_window_close(GTK_WINDOW(item->data));
  }
  g_list_free(windows);
  return G_SOURCE_REMOVE;
}

static cardio::promise<void> receive_batch(Fixture *fixture) {
  // Ready I/O passes through several coroutine continuations before the next
  // blocking read. GTK idle work must not interrupt every one of those steps.
  for (unsigned int index = 0; index < 8; ++index) {
    co_await cardio::from_fd(fixture->descriptors[0], cardio::fd_event::read);
    char byte = 0;
    if (::read(fixture->descriptors[0], &byte, 1) != 1) break;
    ++fixture->completed;
  }
  g_idle_add(close_window, nullptr);
}

static gboolean begin(gpointer context) {
  auto *fixture = static_cast<Fixture *>(context);
  g_idle_add(+[](gpointer context) -> gboolean {
    auto *fixture = static_cast<Fixture *>(context);
    fixture->observed = fixture->completed;
    fixture->observed_idle = true;
    return G_SOURCE_REMOVE;
  }, fixture);
  fixture->work.emplace(receive_batch(fixture));
  return G_SOURCE_REMOVE;
}

extern "C" int __real_main(int argc, char **argv);

// Exercise the actual application entry point and its chosen dispatcher.
// Counting completed operations avoids a machine-speed or timeout assertion.
extern "C" int __wrap_main(int argc, char **argv) {
  auto *directory = g_dir_make_tmp("elder-terms-dispatch-XXXXXX", nullptr);
  if (!directory) return 1;
  g_setenv("XDG_CONFIG_HOME", directory, TRUE);
  Fixture fixture;
  const char bytes[8]{};
  if (::pipe2(fixture.descriptors, O_NONBLOCK | O_CLOEXEC) != 0 ||
      ::write(fixture.descriptors[1], bytes, sizeof(bytes)) != sizeof(bytes)) {
    std::perror("prepare ready pipe");
    for (const auto descriptor : fixture.descriptors) if (descriptor >= 0) ::close(descriptor);
    std::filesystem::remove_all(directory);
    g_free(directory);
    return 1;
  }
  // Wait for initial native allocation and drawing before measuring ordering.
  g_idle_add_full(G_PRIORITY_LOW, begin, &fixture, nullptr);
  const auto status = __real_main(argc, argv);
  ::close(fixture.descriptors[0]);
  ::close(fixture.descriptors[1]);
  std::filesystem::remove_all(directory);
  g_free(directory);
  if (status != 0) return status;
  if (!fixture.observed_idle || fixture.observed != 8 || fixture.completed != 8) {
    std::fprintf(stderr, "GTK idle work interrupted a ready receive batch after %u of 8 operations\n",
        fixture.observed);
    return 1;
  }
  return 0;
}
