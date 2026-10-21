#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <elder-terms/export.h>
#include <elder-terms/settings/settings-store.h>

namespace elder_terms {

/**
 * Prepares one process's desktop identity before its first GTK window exists.
 * @param settings Loaded connection settings; receives the saved connection ID.
 * A temporary launch ID is kept separately and never replaces the saved ID.
 * @param config_path Saved connection INI, if one was selected.
 * @param startup_path Unsaved startup overrides, if present.
 * @param executable Current terminal or file-transfer executable path.
 * @returns True when a connection desktop entry was published. On failure the
 * caller may continue with its ordinary application identity.
 * @remarks Call after gtk_init() and before constructing a GtkWindow. File
 * operations run on a temporary cardio GLib dispatcher using GIO async APIs.
 * A launch without either path keeps the application's existing identity.
 */
ELDER_TERMS_API bool initialize_connection_desktop_session(
    SettingsStore *settings,
    const std::optional<std::filesystem::path> &config_path,
    const std::optional<std::filesystem::path> &startup_path,
    const std::string &executable);

/**
 * Publishes updated name and artwork for the current process's connection.
 * @param settings Effective settings after Apply or Save.
 * @remarks Requires the running cardio GLib dispatcher. Rapid calls cancel
 * older work so an old artwork load cannot replace the latest desktop entry.
 */
ELDER_TERMS_API void update_connection_desktop_session(
    const SettingsStore &settings);

/** Cancels pending desktop updates before the application's dispatcher stops. */
ELDER_TERMS_API void shutdown_connection_desktop_session();

} // namespace elder_terms
