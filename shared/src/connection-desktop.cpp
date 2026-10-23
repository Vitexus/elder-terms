// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/connection-desktop.h>

#include <gio/gio.h>
#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>

namespace elder_terms {

static std::map<std::string, std::weak_ptr<cardio::primitives::mutex>> publication_locks;

std::string connection_application_id(const std::string &id) {
  return g_uuid_string_is_valid(id.c_str())
      ? "net.kekyo.elder-terms.connection." + id : std::string();
}

static cardio::promise<void> ensure_data_directory_async(
    std::filesystem::path path, cardio::cancellation cancellation) {
  if (path.empty() || path == path.root_path()) co_return;
  co_await ensure_data_directory_async(path.parent_path(), cancellation);
  auto file = std::shared_ptr<GFile>(g_file_new_for_path(path.c_str()), g_object_unref);
  auto pending = cardio::gio::submit<gboolean>(
      [file](GCancellable *signal, GAsyncReadyCallback callback, gpointer data) {
        g_file_make_directory_async(file.get(), G_PRIORITY_DEFAULT, signal, callback, data);
      },
      [file](GObject *, GAsyncResult *result, GError **error) {
        return g_file_make_directory_finish(file.get(), result, error);
      }, cancellation);
  try {
    (void)co_await pending;
  } catch (const cardio::gio::gio_error &error) {
    if (error.domain() != G_IO_ERROR || error.code() != G_IO_ERROR_EXISTS) throw;
  }
}

static cardio::promise<void> replace_desktop_file_async(
    std::filesystem::path path, std::shared_ptr<GBytes> bytes,
    cardio::cancellation cancellation) {
  auto file = std::shared_ptr<GFile>(g_file_new_for_path(path.c_str()), g_object_unref);
  auto pending = cardio::gio::submit<gboolean>(
      [file, bytes](GCancellable *signal, GAsyncReadyCallback callback, gpointer data) {
        g_file_replace_contents_bytes_async(file.get(), bytes.get(), nullptr, FALSE,
            G_FILE_CREATE_NONE, signal, callback, data);
      },
      [file](GObject *, GAsyncResult *result, GError **error) {
        return g_file_replace_contents_finish(file.get(), result, nullptr, error);
      }, cancellation);
  (void)co_await pending;
}

static std::string desktop_command(const std::vector<std::string> &arguments) {
  if (arguments.empty() || arguments.front().empty() ||
      arguments.front().find('=') != std::string::npos)
    throw std::invalid_argument("Invalid desktop executable");
  std::string command;
  for (const auto &argument : arguments) {
    if (argument.find('\0') != std::string::npos)
      throw std::invalid_argument("Desktop argument contains a null byte");
    if (!command.empty()) command += ' ';
    command += '"';
    for (const char character : argument) {
      if (character == '\\' || character == '"' || character == '`' || character == '$')
        command += '\\';
      if (character == '%') command += '%';
      command += character;
    }
    command += '"';
  }
  return command;
}

static cardio::promise<std::shared_ptr<GBytes>> load_file_async(
    std::filesystem::path path, cardio::cancellation cancellation) {
  auto file = std::shared_ptr<GFile>(g_file_new_for_path(path.c_str()), g_object_unref);
  auto pending = cardio::gio::submit<GBytes *>(
      [file](GCancellable *signal, GAsyncReadyCallback callback, gpointer data) {
        g_file_load_bytes_async(file.get(), signal, callback, data);
      },
      [file](GObject *, GAsyncResult *result, GError **error) {
        return g_file_load_bytes_finish(file.get(), result, nullptr, error);
      }, cancellation);
  try {
    co_return std::shared_ptr<GBytes>(co_await pending, g_bytes_unref);
  } catch (const cardio::gio::gio_error &error) {
    if (error.domain() == G_IO_ERROR && error.code() == G_IO_ERROR_NOT_FOUND)
      co_return std::shared_ptr<GBytes>();
    throw;
  }
}

static cardio::promise<std::string> check_managed_entry_async(
    std::filesystem::path path, const std::string &id,
    cardio::cancellation cancellation) {
  const auto contents = co_await load_file_async(path, cancellation);
  if (!contents) co_return std::string();
  auto key_file = std::shared_ptr<GKeyFile>(g_key_file_new(), g_key_file_unref);
  gsize size = 0;
  const auto *data = static_cast<const gchar *>(g_bytes_get_data(contents.get(), &size));
  if (!g_key_file_load_from_data(key_file.get(), data, size, G_KEY_FILE_NONE, nullptr))
    throw std::runtime_error("Desktop entry is not managed by elder-terms");
  auto managed_id = std::unique_ptr<gchar, decltype(&g_free)>(g_key_file_get_string(
      key_file.get(), "Desktop Entry", "X-ElderTerms-ConnectionId", nullptr), g_free);
  if (!managed_id || id != managed_id.get())
    throw std::runtime_error("Desktop entry is not managed by elder-terms");
  co_return std::string(data, size);
}

static cardio::promise<void> invalidate_icon_theme_async(
    std::filesystem::path path, cardio::cancellation cancellation) {
  auto file = std::shared_ptr<GFile>(g_file_new_for_path(path.c_str()), g_object_unref);
  auto query = cardio::gio::submit<GFileInfo *>(
      [file](GCancellable *signal, GAsyncReadyCallback callback, gpointer data) {
        g_file_query_info_async(file.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED,
            G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, signal, callback, data);
      },
      [file](GObject *, GAsyncResult *result, GError **error) {
        return g_file_query_info_finish(file.get(), result, error);
      }, cancellation);
  auto info = std::shared_ptr<GFileInfo>(co_await query, g_object_unref);
  // GTK and GNOME compare the theme root's mtime in whole seconds. Advance it
  // even for rapid edits; replacing a PNG in a nested directory is insufficient.
  const auto previous = g_file_info_get_attribute_uint64(info.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED);
  const auto now = static_cast<guint64>(g_get_real_time() / G_USEC_PER_SEC);
  g_file_info_set_attribute_uint64(info.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED,
      std::max(now, previous + 1));
  auto update = cardio::gio::submit<gboolean>(
      [file, info](GCancellable *signal, GAsyncReadyCallback callback, gpointer data) {
        g_file_set_attributes_async(file.get(), info.get(), G_FILE_QUERY_INFO_NONE,
            G_PRIORITY_DEFAULT, signal, callback, data);
      },
      [file](GObject *, GAsyncResult *result, GError **error) {
        GFileInfo *updated = nullptr;
        const auto success = g_file_set_attributes_finish(file.get(), result, &updated, error);
        g_clear_object(&updated);
        return success;
      }, cancellation);
  (void)co_await update;
}

cardio::promise<ConnectionDesktopEntry> publish_connection_desktop_async(
    std::string id, std::string name, std::shared_ptr<GdkPixbuf> pixels,
    std::vector<std::string> command, cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  const auto app_id = connection_application_id(id);
  if (app_id.empty() || !pixels)
    throw std::invalid_argument("Connection desktop identity requires a UUID and image");
  std::erase_if(publication_locks, [](const auto &item) { return item.second.expired(); });
  auto mutex = publication_locks[id].lock();
  if (!mutex) {
    mutex = std::make_shared<cardio::primitives::mutex>();
    publication_locks[id] = mutex;
  }
  auto lock = std::move(co_await mutex->lock(cancellation));
  const auto exec = desktop_command(command);
  const auto data_root = std::filesystem::path(g_get_user_data_dir());
  const auto applications = data_root / "applications";
  const auto theme = data_root / "icons" / "hicolor";
  const auto images = theme / "256x256" / "apps";
  const auto desktop_path = applications / (app_id + ".desktop");
  const auto previous = co_await check_managed_entry_async(desktop_path, id, cancellation);

  gchar *png = nullptr;
  gsize png_size = 0;
  GError *error = nullptr;
  if (!gdk_pixbuf_save_to_buffer(pixels.get(), &png, &png_size, "png", &error, nullptr)) {
    const std::string reason = error ? error->message : "Unable to encode desktop icon";
    g_clear_error(&error);
    throw std::runtime_error(reason);
  }
  auto png_bytes = std::shared_ptr<GBytes>(g_bytes_new_take(png, png_size), g_bytes_unref);
  auto digest = std::unique_ptr<gchar, decltype(&g_free)>(
      g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, png_bytes.get()), g_free);
  const auto icon_path = images / (app_id + ".png");
  co_await ensure_data_directory_async(images, cancellation);
  co_await ensure_data_directory_async(applications, cancellation);
  const auto previous_image = co_await load_file_async(icon_path, cancellation);
  const bool image_changed = !previous_image || !g_bytes_equal(previous_image.get(), png_bytes.get());
  if (image_changed)
    co_await replace_desktop_file_async(icon_path, png_bytes, cancellation);

  auto entry = std::shared_ptr<GKeyFile>(g_key_file_new(), g_key_file_unref);
  g_key_file_set_string(entry.get(), "Desktop Entry", "Type", "Application");
  g_key_file_set_string(entry.get(), "Desktop Entry", "Name", name.c_str());
  g_key_file_set_string(entry.get(), "Desktop Entry", "Exec", exec.c_str());
  // Keep the themed icon name stable so running ShellApps can refresh their
  // retained GIcon when the theme changes, without reopening the window.
  g_key_file_set_string(entry.get(), "Desktop Entry", "Icon", app_id.c_str());
  g_key_file_set_string(entry.get(), "Desktop Entry", "StartupWMClass", app_id.c_str());
  g_key_file_set_boolean(entry.get(), "Desktop Entry", "NoDisplay", TRUE);
  g_key_file_set_boolean(entry.get(), "Desktop Entry", "Terminal", FALSE);
  g_key_file_set_string(entry.get(), "Desktop Entry", "X-ElderTerms-ConnectionId", id.c_str());
  // An icon-only edit must also notify desktop-entry monitors to rescan themes.
  g_key_file_set_string(entry.get(), "Desktop Entry", "X-ElderTerms-IconDigest", digest.get());
  gsize entry_size = 0;
  auto *entry_data = g_key_file_to_data(entry.get(), &entry_size, nullptr);
  auto entry_bytes = std::shared_ptr<GBytes>(g_bytes_new_take(entry_data, entry_size), g_bytes_unref);
  const bool entry_changed = previous != std::string(entry_data, entry_size);
  // Also retry invalidation if a previous publication stopped after writing PNG.
  if (image_changed || entry_changed) {
    co_await invalidate_icon_theme_async(theme, cancellation);
    co_await replace_desktop_file_async(desktop_path, entry_bytes, cancellation);
  }
  co_return {app_id, desktop_path, icon_path};
}

} // namespace elder_terms
