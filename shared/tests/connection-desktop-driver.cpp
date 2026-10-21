#include <elder-terms/connection-desktop.h>
#include <cardio.h>
#include <iostream>
#include <string>
#include <vector>

static cardio::promise<void> publish(int argc, char **argv,
    cardio::dispatcher_group_glib *group, std::exception_ptr *failure) {
  try {
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
  if (argc < 5) return 2;
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
