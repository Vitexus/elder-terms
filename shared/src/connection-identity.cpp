#include <elder-terms/connection-identity.h>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <gio/gio.h>

namespace elder_terms {

struct ConnectionIdentityContents {
  gchar *data = nullptr;
  gsize size = 0;
  gchar *etag = nullptr;

  ConnectionIdentityContents() = default;
  ConnectionIdentityContents(const ConnectionIdentityContents &) = delete;
  ConnectionIdentityContents &operator=(const ConnectionIdentityContents &) = delete;
  ~ConnectionIdentityContents() {
    g_free(data);
    g_free(etag);
  }
};

struct ConnectionIdentityFileGeneration {
  std::string file_id;
  std::string filesystem_id;
  std::string etag;
};

static std::mutex connection_identity_mutex_map_guard;
static std::unordered_map<std::string, std::weak_ptr<cardio::primitives::mutex>>
    connection_identity_mutexes;

static std::shared_ptr<cardio::primitives::mutex>
connection_identity_mutex(const std::filesystem::path &path) {
  const auto key = std::filesystem::absolute(path).lexically_normal().string();
  const auto guard = std::lock_guard(connection_identity_mutex_map_guard);
  auto &entry = connection_identity_mutexes[key];
  auto mutex = entry.lock();
  if (mutex == nullptr) {
    mutex = std::make_shared<cardio::primitives::mutex>();
    entry = mutex;
    for (auto iterator = connection_identity_mutexes.begin();
         iterator != connection_identity_mutexes.end();) {
      if (iterator->first != key && iterator->second.expired())
        iterator = connection_identity_mutexes.erase(iterator);
      else
        ++iterator;
    }
  }
  return mutex;
}

static std::string connection_identity_for_legacy_file(
    const std::filesystem::path &path,
    const ConnectionIdentityFileGeneration &generation,
    const ConnectionIdentityContents &contents) {
  // RFC 9562 section 5.8 assigns SHA-256 name-based UUIDs to version 8.
  // Include the file generation so a new profile reusing the same path does
  // not inherit the old identity. Concurrent readers of one generation still
  // choose the same ID if an entity-tag check precedes its atomic rename.
  const auto normalized = std::filesystem::absolute(path).lexically_normal();
  auto content_checksum = std::unique_ptr<gchar, decltype(&g_free)>(
      g_compute_checksum_for_data(
          G_CHECKSUM_SHA256, reinterpret_cast<const guchar *>(contents.data),
          contents.size), g_free);
  if (content_checksum == nullptr)
    throw std::runtime_error("Failed to hash connection profile");
  const auto source = std::string("elder-terms:connection-id:legacy:v2:") +
                      normalized.string() + "\n" + generation.filesystem_id +
                      "\n" + generation.file_id + "\n" + generation.etag +
                      "\n" + content_checksum.get();
  auto checksum = std::unique_ptr<gchar, decltype(&g_free)>(
      g_compute_checksum_for_string(G_CHECKSUM_SHA256, source.c_str(),
                                    source.size()), g_free);
  if (checksum == nullptr)
    throw std::runtime_error("Failed to derive connection ID");
  std::string digits(checksum.get(), 32);
  digits[12] = '8';
  const auto variant = static_cast<unsigned int>(
      g_ascii_xdigit_value(digits[16]));
  digits[16] = "89ab"[variant & 3U];
  return digits.substr(0, 8) + "-" + digits.substr(8, 4) + "-" +
         digits.substr(12, 4) + "-" + digits.substr(16, 4) + "-" +
         digits.substr(20, 12);
}

std::string generate_connection_id() {
  auto uuid = std::unique_ptr<gchar, decltype(&g_free)>(g_uuid_string_random(),
                                                        g_free);
  if (uuid == nullptr) throw std::runtime_error("Failed to create connection ID");
  return uuid.get();
}

static cardio::promise<std::shared_ptr<ConnectionIdentityContents>>
read_connection_contents_async(std::shared_ptr<GFile> file,
                               cardio::cancellation cancellation) {
  auto contents = std::make_shared<ConnectionIdentityContents>();
  auto task = cardio::gio::submit<gboolean>(
      [file](GCancellable *signal, GAsyncReadyCallback callback,
             gpointer data) {
        g_file_load_contents_async(file.get(), signal, callback, data);
      },
      [file, contents](GObject *, GAsyncResult *result, GError **error) {
        return g_file_load_contents_finish(file.get(), result, &contents->data,
                                           &contents->size, &contents->etag,
                                           error);
      }, cancellation);
  (void)co_await task;
  co_return contents;
}

static cardio::promise<ConnectionIdentityFileGeneration>
query_connection_file_generation_async(std::shared_ptr<GFile> file,
                                       cardio::cancellation cancellation) {
  auto task = cardio::gio::submit<GFileInfo *>(
      [file](GCancellable *signal, GAsyncReadyCallback callback,
             gpointer data) {
        g_file_query_info_async(
            file.get(), G_FILE_ATTRIBUTE_ID_FILE ","
                            G_FILE_ATTRIBUTE_ID_FILESYSTEM ","
                            G_FILE_ATTRIBUTE_ETAG_VALUE,
            G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, signal, callback,
            data);
      },
      [file](GObject *, GAsyncResult *result, GError **error) {
        return g_file_query_info_finish(file.get(), result, error);
      }, cancellation);
  auto info = std::shared_ptr<GFileInfo>(co_await task, g_object_unref);
  const char *file_id = g_file_info_get_attribute_string(
      info.get(), G_FILE_ATTRIBUTE_ID_FILE);
  const char *filesystem_id = g_file_info_get_attribute_string(
      info.get(), G_FILE_ATTRIBUTE_ID_FILESYSTEM);
  const char *etag = g_file_info_get_attribute_string(
      info.get(), G_FILE_ATTRIBUTE_ETAG_VALUE);
  if (file_id == nullptr || *file_id == '\0' ||
      filesystem_id == nullptr || *filesystem_id == '\0' ||
      etag == nullptr || *etag == '\0')
    throw std::runtime_error("Connection profile has no file identity");
  co_return ConnectionIdentityFileGeneration{file_id, filesystem_id, etag};
}

cardio::promise<std::string> ensure_connection_id_async(
    std::filesystem::path path, cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  auto mutex = connection_identity_mutex(path);
  auto lock = std::move(co_await mutex->lock(cancellation));
  auto file = std::shared_ptr<GFile>(g_file_new_for_path(path.c_str()),
                                     g_object_unref);
  for (int attempt = 0; attempt < 8; ++attempt) {
    auto contents = co_await read_connection_contents_async(file, cancellation);
    auto key_file = std::shared_ptr<GKeyFile>(g_key_file_new(),
                                              g_key_file_unref);
    GError *parse_error = nullptr;
    if (!g_key_file_load_from_data(
            key_file.get(), contents->data, contents->size,
            static_cast<GKeyFileFlags>(G_KEY_FILE_KEEP_COMMENTS |
                                       G_KEY_FILE_KEEP_TRANSLATIONS),
            &parse_error)) {
      const std::string message = parse_error == nullptr
                                      ? "Failed to parse connection profile"
                                      : parse_error->message;
      g_clear_error(&parse_error);
      throw std::runtime_error(message);
    }
    auto current = std::unique_ptr<gchar, decltype(&g_free)>(
        g_key_file_get_string(key_file.get(), "general", "connection_id",
                              nullptr),
        g_free);
    if (current != nullptr && g_uuid_string_is_valid(current.get())) {
      cancellation.throw_if_cancellation_requested();
      co_return std::string(current.get());
    }
    const auto generation = co_await query_connection_file_generation_async(
        file, cancellation);
    if (contents->etag == nullptr || generation.etag != contents->etag)
      continue;
    const auto current_generation = co_await query_connection_file_generation_async(
        file, cancellation);
    if (generation.file_id != current_generation.file_id ||
        generation.filesystem_id != current_generation.filesystem_id ||
        generation.etag != current_generation.etag)
      continue;
    const auto new_id = connection_identity_for_legacy_file(
        path, generation, *contents);
    g_key_file_set_string(key_file.get(), "general", "connection_id",
                          new_id.c_str());
    gsize replacement_size = 0;
    auto replacement = std::unique_ptr<gchar, decltype(&g_free)>(
        g_key_file_to_data(key_file.get(), &replacement_size, nullptr), g_free);
    if (replacement == nullptr)
      throw std::runtime_error("Failed to serialize connection profile");
    auto bytes = std::shared_ptr<GBytes>(
        g_bytes_new(replacement.get(), replacement_size), g_bytes_unref);
    auto task = cardio::gio::submit<gboolean>(
        [file, bytes, contents](GCancellable *signal,
                                 GAsyncReadyCallback callback, gpointer data) {
          g_file_replace_contents_bytes_async(
              file.get(), bytes.get(), contents->etag, FALSE,
              G_FILE_CREATE_NONE, signal, callback, data);
        },
        [file](GObject *, GAsyncResult *result, GError **error) {
          return g_file_replace_contents_finish(file.get(), result, nullptr,
                                                error);
        }, cancellation);
    try {
      (void)co_await task;
      co_return new_id;
    } catch (const cardio::gio::gio_error &error) {
      if (error.domain() != G_IO_ERROR || error.code() != G_IO_ERROR_WRONG_ETAG)
        throw;
    }
  }
  throw std::runtime_error("Connection profile changed repeatedly");
}

} // namespace elder_terms
