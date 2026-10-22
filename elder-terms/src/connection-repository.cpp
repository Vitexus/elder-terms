// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "connection-repository.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <system_error>
#include <utility>

#include <glib.h>
#include <unistd.h>

#include <elder-terms/connection-identity.h>
#include <elder-terms/settings/general-settings.h>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

static constexpr gdouble default_terminal_font_size_dip = 12.0;
static constexpr char initial_local_terminal_name[] = "Local terminal";

bool connection_profile_only_gained_id(
    const std::optional<std::string> &before,
    const std::optional<std::string> &after) {
  if (!before || !after) return false;
  auto previous = std::shared_ptr<GKeyFile>(g_key_file_new(), g_key_file_unref);
  auto current = std::shared_ptr<GKeyFile>(g_key_file_new(), g_key_file_unref);
  const auto flags = static_cast<GKeyFileFlags>(
      G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS);
  if (!g_key_file_load_from_data(previous.get(), before->data(), before->size(),
                                 flags, nullptr) ||
      !g_key_file_load_from_data(current.get(), after->data(), after->size(),
                                 flags, nullptr))
    return false;
  auto old_id = std::unique_ptr<gchar, decltype(&g_free)>(
      g_key_file_get_string(previous.get(), "general", "connection_id", nullptr),
      g_free);
  auto new_id = std::unique_ptr<gchar, decltype(&g_free)>(
      g_key_file_get_string(current.get(), "general", "connection_id", nullptr),
      g_free);
  if (old_id && g_uuid_string_is_valid(old_id.get())) return false;
  if (!new_id || !g_uuid_string_is_valid(new_id.get())) return false;
  const bool had_general = g_key_file_has_group(previous.get(), "general");
  (void)g_key_file_remove_key(previous.get(), "general", "connection_id", nullptr);
  (void)g_key_file_remove_key(current.get(), "general", "connection_id", nullptr);
  if (!had_general) {
    gsize remaining_keys = 0;
    auto keys = std::unique_ptr<gchar *, decltype(&g_strfreev)>(
        g_key_file_get_keys(current.get(), "general", &remaining_keys, nullptr),
        g_strfreev);
    if (remaining_keys == 0)
      (void)g_key_file_remove_group(current.get(), "general", nullptr);
  }
  gsize previous_size = 0;
  gsize current_size = 0;
  auto previous_text = std::unique_ptr<gchar, decltype(&g_free)>(
      g_key_file_to_data(previous.get(), &previous_size, nullptr), g_free);
  auto current_text = std::unique_ptr<gchar, decltype(&g_free)>(
      g_key_file_to_data(current.get(), &current_size, nullptr), g_free);
  return previous_text && current_text && previous_size == current_size &&
         std::memcmp(previous_text.get(), current_text.get(), previous_size) == 0;
}

static std::string trim_ascii_whitespace(const std::string &value) {
  const auto first = std::find_if_not(
      value.begin(), value.end(),
      [](unsigned char character) { return g_ascii_isspace(character); });
  const auto last = std::find_if_not(
      value.rbegin(), value.rend(),
      [](unsigned char character) { return g_ascii_isspace(character); })
                        .base();
  return first < last ? std::string(first, last) : std::string();
}

static std::string file_error(const char *format,
                              const std::filesystem::path &path,
                              const std::error_code &error) {
  gchar *formatted = g_strdup_printf(
      format, path.string().c_str(), error.message().c_str());
  const std::string result = formatted == nullptr ? std::string() : formatted;
  g_free(formatted);
  return result;
}

static std::string temporary_file_error(const char *format,
                                        const std::string &path,
                                        int error) {
  gchar *formatted =
      g_strdup_printf(format, path.c_str(), std::strerror(error));
  const std::string result = formatted == nullptr ? std::string() : formatted;
  g_free(formatted);
  return result;
}

static std::optional<std::filesystem::path> create_temporary_file(
    const std::filesystem::path &directory, const char *kind,
    std::vector<std::string> *warnings) {
  std::string template_path =
      (directory / (std::string(".elder-terms-") + kind + "-XXXXXX.ini"))
          .string();
  const int descriptor = g_mkstemp(template_path.data());
  if (descriptor < 0) {
    const int failure = errno;
    warnings->push_back(temporary_file_error(
        _("Warning: failed to create temporary file %s: %s"), template_path,
        failure));
    return std::nullopt;
  }
  if (close(descriptor) != 0) {
    const int failure = errno;
    warnings->push_back(temporary_file_error(
        _("Warning: failed to close temporary file %s: %s"), template_path,
        failure));
    std::error_code remove_error;
    std::filesystem::remove(template_path, remove_error);
    return std::nullopt;
  }
  return std::filesystem::path(template_path);
}

static void remove_temporary_file(const std::filesystem::path &path) {
  std::error_code error;
  std::filesystem::remove(path, error);
}

std::filesystem::path default_connection_directory() {
  return std::filesystem::path(g_get_user_config_dir()) / "elder-terms" /
         "connections";
}

InitialConnectionProfileResult create_initial_local_terminal_profile(
    const std::filesystem::path &directory) {
  InitialConnectionProfileResult result;
  std::error_code exists_error;
  if (std::filesystem::exists(directory, exists_error)) {
    return result;
  }
  if (exists_error) {
    result.warnings.push_back(file_error(
        _("Warning: failed to inspect connection directory %s: %s"),
        directory, exists_error));
    return result;
  }

  const SettingsStore store = create_default_settings(
      default_terminal_display_settings(default_terminal_font_size_dip),
      initial_local_terminal_name);
  ConnectionSaveResult saved = save_connection_profile(
      directory, std::nullopt, initial_local_terminal_name, store);
  result.created = saved.saved;
  result.path = std::move(saved.path);
  result.warnings = std::move(saved.warnings);
  return result;
}

std::vector<ConnectionProfile>
list_connection_profiles(const std::filesystem::path &directory) {
  std::vector<ConnectionProfile> profiles;
  std::error_code iterator_error;
  std::filesystem::directory_iterator iterator(directory, iterator_error);
  if (iterator_error) {
    return profiles;
  }

  for (const std::filesystem::directory_entry &entry : iterator) {
    std::error_code type_error;
    if (!entry.is_regular_file(type_error) || type_error ||
        entry.path().extension() != ".ini") {
      continue;
    }
    profiles.push_back({
        .name = entry.path().stem().string(),
        .path = entry.path(),
    });
  }
  std::sort(profiles.begin(), profiles.end(),
            [](const ConnectionProfile &left,
               const ConnectionProfile &right) {
              return left.name < right.name;
            });
  return profiles;
}

ConnectionNameValidationResult validate_connection_name(
    const std::string &candidate,
    const std::vector<ConnectionProfile> &profiles,
    const std::optional<std::filesystem::path> &current_path) {
  ConnectionNameValidationResult result{
      .valid = false,
      .name = trim_ascii_whitespace(candidate),
      .error = {},
  };
  if (result.name.empty()) {
    result.error = _("Connection name must not be empty");
    return result;
  }
  if (result.name == "." || result.name == "..") {
    result.error = _("Connection name must not be a relative path");
    return result;
  }
  if (result.name.find('/') != std::string::npos ||
      result.name.find('\0') != std::string::npos) {
    result.error = _("Connection name must not contain a path separator");
    return result;
  }

  const auto duplicate = std::find_if(
      profiles.begin(), profiles.end(),
      [&result, &current_path](const ConnectionProfile &profile) {
        return profile.name == result.name &&
               (!current_path.has_value() ||
                profile.path != current_path.value());
      });
  if (duplicate != profiles.end()) {
    result.error = _("A connection with this name already exists");
    return result;
  }
  result.valid = true;
  return result;
}

SettingsLoadResult load_connection_profile(const std::filesystem::path &path) {
  return load_settings(
      SettingsLoadOptions{
          .config_path = path,
          .startup_config_path = std::nullopt,
          .global_config_path = default_global_config_path(),
      },
      default_terminal_font_size_dip);
}

ConnectionSaveResult save_connection_profile(
    const std::filesystem::path &directory,
    const std::optional<std::filesystem::path> &original_path,
    const std::string &name, const SettingsStore &store) {
  ConnectionSaveResult result;
  std::error_code directory_error;
  std::filesystem::create_directories(directory, directory_error);
  if (directory_error) {
    result.warnings.push_back(
        file_error(_("Warning: failed to create connection directory %s: %s"),
                   directory, directory_error));
    return result;
  }

  const auto profiles = list_connection_profiles(directory);
  const ConnectionNameValidationResult validation =
      validate_connection_name(name, profiles, original_path);
  if (!validation.valid) {
    gchar *formatted =
        g_strdup_printf(_("Warning: %s"), validation.error.c_str());
    result.warnings.emplace_back(formatted == nullptr ? "" : formatted);
    g_free(formatted);
    return result;
  }

  const std::filesystem::path target =
      directory / (validation.name + ".ini");
  const auto temporary =
      create_temporary_file(directory, "save", &result.warnings);
  if (!temporary.has_value()) {
    return result;
  }
  SettingsStore saved_store = store;
  std::string id;
  if (original_path.has_value()) {
    const auto existing = load_connection_profile(*original_path);
    if (existing.loaded) id = general_connection_id(existing.store);
  }
  if (id.empty()) id = generate_connection_id();
  if (!set_explicit_setting_value(&saved_store,
                                  general_connection_id_setting_key(), id)) {
    result.warnings.push_back("Warning: failed to set connection ID");
    remove_temporary_file(*temporary);
    return result;
  }
  const SettingsSaveResult save_result = save_settings(saved_store, *temporary);
  result.warnings.insert(result.warnings.end(), save_result.warnings.begin(),
                         save_result.warnings.end());
  if (!save_result.saved) {
    remove_temporary_file(*temporary);
    return result;
  }

  if (!original_path.has_value() || original_path.value() == target) {
    std::error_code replace_error;
    std::filesystem::rename(*temporary, target, replace_error);
    if (replace_error) {
      result.warnings.push_back(
          file_error(_("Warning: failed to replace connection profile %s: %s"),
                     target, replace_error));
      remove_temporary_file(*temporary);
      return result;
    }
    result.saved = true;
    result.path = target;
    return result;
  }

  const auto backup =
      create_temporary_file(directory, "backup", &result.warnings);
  if (!backup.has_value()) {
    remove_temporary_file(*temporary);
    return result;
  }
  std::error_code backup_error;
  std::filesystem::rename(original_path.value(), *backup, backup_error);
  if (backup_error) {
    result.warnings.push_back(file_error(
        _("Warning: failed to prepare connection rename %s: %s"),
        original_path.value(), backup_error));
    remove_temporary_file(*temporary);
    remove_temporary_file(*backup);
    return result;
  }

  std::error_code replace_error;
  std::filesystem::rename(*temporary, target, replace_error);
  if (replace_error) {
    result.warnings.push_back(
        file_error(_("Warning: failed to rename connection profile %s: %s"),
                   target, replace_error));
    std::error_code rollback_error;
    std::filesystem::rename(*backup, original_path.value(), rollback_error);
    if (rollback_error) {
      result.warnings.push_back(file_error(
          _("Warning: failed to restore connection profile %s: %s"),
          original_path.value(), rollback_error));
    }
    remove_temporary_file(*temporary);
    remove_temporary_file(*backup);
    return result;
  }

  remove_temporary_file(*backup);
  result.saved = true;
  result.path = target;
  return result;
}

ConnectionRenameResult rename_connection_profile(
    const std::filesystem::path &directory,
    const std::filesystem::path &original_path, const std::string &name) {
  ConnectionRenameResult result;
  const auto profiles = list_connection_profiles(directory);
  const ConnectionNameValidationResult validation =
      validate_connection_name(name, profiles, original_path);
  if (!validation.valid) {
    gchar *formatted =
        g_strdup_printf(_("Warning: %s"), validation.error.c_str());
    result.warnings.emplace_back(formatted == nullptr ? "" : formatted);
    g_free(formatted);
    return result;
  }

  const std::filesystem::path target =
      directory / (validation.name + ".ini");
  if (original_path == target) {
    std::error_code type_error;
    if (std::filesystem::is_regular_file(original_path, type_error) &&
        !type_error) {
      result.renamed = true;
      result.path = target;
      return result;
    }
  }

  std::error_code rename_error;
  std::filesystem::rename(original_path, target, rename_error);
  if (rename_error) {
    result.warnings.push_back(
        file_error(_("Warning: failed to rename connection profile %s: %s"),
                   target, rename_error));
    return result;
  }
  result.renamed = true;
  result.path = target;
  return result;
}

ConnectionDeleteResult
delete_connection_profile(const std::filesystem::path &path) {
  ConnectionDeleteResult result;
  std::error_code delete_error;
  const bool deleted = std::filesystem::remove(path, delete_error);
  if (!deleted || delete_error) {
    if (!delete_error) {
      delete_error = std::make_error_code(
          std::errc::no_such_file_or_directory);
    }
    result.warnings.push_back(
        file_error(_("Warning: failed to delete connection profile %s: %s"),
                   path, delete_error));
    return result;
  }
  result.deleted = true;
  return result;
}

} // namespace elder_terms
