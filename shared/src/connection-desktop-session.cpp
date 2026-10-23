// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/connection-desktop-session.h>

#include <algorithm>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gio/gio.h>
#include <gdk/gdk.h>
#include <cardio.h>

#include <elder-terms/connection-desktop.h>
#include <elder-terms/connection-identity.h>
#include <elder-terms/connection-icon.h>
#include <elder-terms/settings/general-settings.h>

namespace elder_terms {

struct ConnectionDesktopSessionState {
  std::string id;
  std::vector<std::string> command;
  cardio::cancellation_source current_cancellation;
  std::vector<cardio::promise<void>> updates;
  cardio::primitives::mutex publication_mutex;
  std::size_t generation = 0;
};

static ConnectionDesktopSessionState connection_desktop_session;

static cardio::promise<bool> executable_exists_async(
    const std::filesystem::path &path, cardio::cancellation cancellation) {
  auto file = std::shared_ptr<GFile>(g_file_new_for_path(path.c_str()),
                                     g_object_unref);
  auto task = cardio::gio::submit<GFileInfo *>(
      [file](GCancellable *signal, GAsyncReadyCallback callback,
             gpointer data) {
        g_file_query_info_async(
            file.get(), G_FILE_ATTRIBUTE_STANDARD_TYPE,
            G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, signal, callback,
            data);
      },
      [file](GObject *, GAsyncResult *result, GError **error) {
        return g_file_query_info_finish(file.get(), result, error);
      }, cancellation);
  try {
    auto info = std::shared_ptr<GFileInfo>(co_await task, g_object_unref);
    co_return g_file_info_get_file_type(info.get()) == G_FILE_TYPE_REGULAR;
  } catch (const cardio::gio::gio_error &error) {
    if (error.domain() == G_IO_ERROR && error.code() == G_IO_ERROR_NOT_FOUND)
      co_return false;
    throw;
  }
}

static cardio::promise<std::filesystem::path> current_executable_async(
    const std::string &fallback, cardio::cancellation cancellation) {
  auto file = std::shared_ptr<GFile>(
      g_file_new_for_path("/proc/self/exe"), g_object_unref);
  auto task = cardio::gio::submit<GFileInfo *>(
      [file](GCancellable *signal, GAsyncReadyCallback callback,
             gpointer data) {
        g_file_query_info_async(
            file.get(), G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_DEFAULT,
            signal, callback, data);
      },
      [file](GObject *, GAsyncResult *result, GError **error) {
        return g_file_query_info_finish(file.get(), result, error);
      }, cancellation);
  try {
    auto info = std::shared_ptr<GFileInfo>(co_await task, g_object_unref);
    if (g_file_info_has_attribute(
            info.get(), G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET)) {
      const char *target = g_file_info_get_symlink_target(info.get());
      if (target != nullptr && *target != '\0')
        co_return std::filesystem::path(target);
    }
  } catch (const cardio::gio::gio_error &) {
  }
  co_return std::filesystem::absolute(fallback).lexically_normal();
}

static cardio::promise<std::vector<std::string>> desktop_command_async(
    const std::optional<std::filesystem::path> &config_path,
    const std::optional<std::filesystem::path> &startup_path,
    const std::string &id, const std::string &executable,
    cardio::cancellation cancellation) {
  const auto application = co_await current_executable_async(executable,
                                                              cancellation);
  const auto local_controller =
      application.parent_path().parent_path() / "elder-terms" / "etctl";
  const bool local_controller_exists =
      co_await executable_exists_async(local_controller, cancellation);
  const auto controller = local_controller_exists
                              ? local_controller.string() : std::string("etctl");
  const auto launcher = local_controller_exists
                            ? (local_controller.parent_path() / "elder-terms").string()
                            : std::string("elder-terms");
  if (startup_path.has_value())
    co_return std::vector<std::string>{launcher};
  if (config_path.has_value()) {
    const auto profile =
        std::filesystem::absolute(*config_path).lexically_normal();
    const auto repository =
        std::filesystem::absolute(std::filesystem::path(g_get_user_config_dir()) /
                                  "elder-terms" / "connections")
            .lexically_normal();
    if (profile.parent_path() == repository && profile.extension() == ".ini")
      co_return std::vector<std::string>{controller, "open-connection-id", id};
    co_return std::vector<std::string>{application.string(), "-c", profile.string()};
  }
  co_return std::vector<std::string>{launcher};
}

static cardio::promise<void> publish_session_async(
    SettingsStore settings, std::string id, std::vector<std::string> command,
    cardio::cancellation cancellation) {
  const auto icon = co_await load_connection_icon_async(
      general_connection_icon(settings), cancellation);
  (void)co_await publish_connection_desktop_async(
      std::move(id), general_connection_name(settings), icon.image,
      std::move(command), cancellation);
}

static cardio::promise<void> initialize_session_async(
    SettingsStore *settings,
    std::optional<std::filesystem::path> config_path,
    std::optional<std::filesystem::path> startup_path,
    std::string executable, cardio::dispatcher_group_glib *group,
    std::exception_ptr *failure) {
  try {
    std::string id;
    if (startup_path.has_value()) {
      id = generate_connection_id();
    } else if (config_path.has_value()) {
      id = co_await ensure_connection_id_async(*config_path, {});
    }
    if (!id.empty()) {
      if (!startup_path.has_value() &&
          !set_explicit_setting_value(
              settings, general_connection_id_setting_key(), id))
        throw std::runtime_error("Failed to set connection ID");
      auto command = co_await desktop_command_async(
          config_path, startup_path, id, executable, {});
      co_await publish_session_async(*settings, id, command, {});
      connection_desktop_session.id = std::move(id);
      connection_desktop_session.command = std::move(command);
    }
  } catch (...) {
    *failure = std::current_exception();
  }
  group->shutdown();
}

bool initialize_connection_desktop_session(
    SettingsStore *settings,
    const std::optional<std::filesystem::path> &config_path,
    const std::optional<std::filesystem::path> &startup_path,
    const std::string &executable) {
  if (settings == nullptr ||
      (!config_path.has_value() && !startup_path.has_value()))
    return false;
  std::exception_ptr failure;
  {
    cardio::dispatcher_group_glib group;
    cardio::dispatcher_host_glib dispatcher(group);
    auto task = initialize_session_async(settings, config_path, startup_path,
                                         executable, &group, &failure);
    dispatcher.park();
  }
  if (failure != nullptr) {
    try {
      std::rethrow_exception(failure);
    } catch (const std::exception &error) {
      std::cerr << "Warning: connection desktop identity: " << error.what()
                << '\n';
    }
    return false;
  }
  if (connection_desktop_session.id.empty()) return false;
  const auto application_id =
      connection_application_id(connection_desktop_session.id);
  g_set_prgname(application_id.c_str());
  gdk_set_program_class(application_id.c_str());
  return true;
}

static cardio::promise<void> update_session_async(
    SettingsStore settings, std::string id, std::vector<std::string> command,
    std::size_t generation, cardio::cancellation cancellation) {
  try {
    const auto icon = co_await load_connection_icon_async(
        general_connection_icon(settings), cancellation);
    // Serialize publications so a canceled older write cannot finish after
    // the latest desktop entry and overwrite it.
    auto lock = std::move(co_await
        connection_desktop_session.publication_mutex.lock(cancellation));
    if (generation != connection_desktop_session.generation) co_return;
    (void)co_await publish_connection_desktop_async(
        std::move(id), general_connection_name(settings), icon.image,
        std::move(command), cancellation);
  } catch (const cardio::canceled_exception &) {
  } catch (const std::exception &error) {
    std::cerr << "Warning: connection desktop update: " << error.what()
              << '\n';
  }
}

void update_connection_desktop_session(const SettingsStore &settings) {
  if (connection_desktop_session.id.empty()) return;
  (void)connection_desktop_session.current_cancellation.cancel();
  connection_desktop_session.current_cancellation =
      cardio::cancellation_source();
  std::erase_if(connection_desktop_session.updates,
                [](const auto &task) { return task.is_ready(); });
  const auto generation = ++connection_desktop_session.generation;
  connection_desktop_session.updates.emplace_back(update_session_async(
      settings, connection_desktop_session.id,
      connection_desktop_session.command, generation,
      connection_desktop_session.current_cancellation.get_cancellation()));
}

void shutdown_connection_desktop_session() {
  (void)connection_desktop_session.current_cancellation.cancel();
  connection_desktop_session.updates.clear();
  connection_desktop_session.id.clear();
  connection_desktop_session.command.clear();
  ++connection_desktop_session.generation;
}

} // namespace elder_terms
