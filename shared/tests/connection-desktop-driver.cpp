// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/connection-desktop.h>
#include <cardio.h>
#include <gtk/gtk.h>
#include <iostream>
#include <string>
#include <vector>

static void expect_icon_color(GtkIconTheme *theme, GIcon *icon, guint32 color) {
  auto info = std::shared_ptr<GtkIconInfo>(gtk_icon_theme_lookup_by_gicon(
      theme, icon, 32, GTK_ICON_LOOKUP_FORCE_SIZE), g_object_unref);
  if (!info) throw std::runtime_error("Desktop icon cannot be resolved");
  auto pixels = std::shared_ptr<GdkPixbuf>(
      gtk_icon_info_load_icon(info.get(), nullptr), g_object_unref);
  if (!pixels) throw std::runtime_error("Desktop icon cannot be loaded");
  const auto *pixel = gdk_pixbuf_read_pixels(pixels.get());
  if (pixel[0] != (color >> 24) || pixel[1] != ((color >> 16) & 255) ||
      pixel[2] != ((color >> 8) & 255))
    throw std::runtime_error("Cached desktop icon still displays the previous image");
}

static cardio::promise<void> check_cached_icon() {
  const std::string id = "853bb765-3194-4549-a469-02e3220c6c0e";
  auto pixels = std::shared_ptr<GdkPixbuf>(
      gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 256, 256), g_object_unref);
  gdk_pixbuf_fill(pixels.get(), 0xff0000ff);
  const auto entry = co_await elder_terms::publish_connection_desktop_async(
      id, "Cached connection", pixels, {"true"}, {});
  auto desktop = std::shared_ptr<GKeyFile>(g_key_file_new(), g_key_file_unref);
  if (!g_key_file_load_from_file(desktop.get(), entry.desktop_path.c_str(),
          G_KEY_FILE_NONE, nullptr))
    throw std::runtime_error("Desktop entry cannot be loaded");
  auto name = std::unique_ptr<gchar, decltype(&g_free)>(
      g_key_file_get_string(desktop.get(), "Desktop Entry", "Icon", nullptr), g_free);
  auto icon = std::shared_ptr<GIcon>(g_icon_new_for_string(name.get(), nullptr), g_object_unref);
  auto *theme = gtk_icon_theme_get_default();
  expect_icon_color(theme, icon.get(), 0xff0000ff);
  // Retain the original desktop icon, as a running GNOME Shell app does.
  // Consecutive updates must work even within one filesystem timestamp second.
  for (const guint32 color : {0x0000ffffU, 0x00ff00ffU, 0xff0000ffU}) {
    gdk_pixbuf_fill(pixels.get(), color);
    (void)co_await elder_terms::publish_connection_desktop_async(
        id, "Cached connection", pixels, {"true"}, {});
    gtk_icon_theme_rescan_if_needed(theme);
    expect_icon_color(theme, icon.get(), color);
  }
}

static cardio::promise<void> publish(int argc, char **argv,
    cardio::dispatcher_group_glib *group, std::exception_ptr *failure) {
  try {
    if (std::string(argv[1]) == "--check-cached-icon") {
      co_await check_cached_icon();
      group->shutdown();
      co_return;
    }
    auto pixels = std::shared_ptr<GdkPixbuf>(
        gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 256, 256), g_object_unref);
    gdk_pixbuf_fill(pixels.get(), std::stoul(argv[3], nullptr, 16));
    std::vector<std::string> command;
    for (int index = 4; index < argc; ++index) command.emplace_back(argv[index]);
    const auto entry = co_await elder_terms::publish_connection_desktop_async(
        argv[1], argv[2], pixels, command, {});
    std::cout << entry.desktop_path.string() << '\n' << entry.icon_path.string() << '\n';
  } catch (...) {
    *failure = std::current_exception();
  }
  group->shutdown();
}

int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--check-cached-icon")
    gtk_init(nullptr, nullptr);
  else if (argc < 5) return 2;
  std::exception_ptr failure;
  {
    cardio::dispatcher_group_glib group;
    cardio::dispatcher_host_glib dispatcher(group);
    auto task = publish(argc, argv, &group, &failure);
    dispatcher.park();
  }
  if (failure) {
    try { std::rethrow_exception(failure); }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
    return 1;
  }
  return 0;
}
