// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <filesystem>
#include <string>

#include <cardio.h>

#include <elder-terms/export.h>

namespace elder_terms {

/**
 * Generates a new random UUID for a connection profile.
 * @returns RFC 4122 version 4 UUID string.
 */
ELDER_TERMS_API std::string generate_connection_id();

/**
 * Ensures an existing connection profile has one durable UUID.
 * @param path Existing local INI profile path.
 * @param cancellation Signal that cancels GIO file operations.
 * @returns The existing or newly persisted UUID. A missing or invalid ID is
 * derived from the normalized absolute path, file identity, entity tag, and
 * original contents. Concurrent processes reading one file generation choose
 * the same replacement; a new file reusing its path receives a different ID.
 * @throws cardio::canceled_exception if canceled; an exception for read,
 * parse, or write failures.
 * @remarks Uses the current cardio GLib dispatcher and GIO asynchronous I/O.
 * Other INI keys and comments are retained when a missing/invalid ID is fixed.
 * Different path aliases to one file are outside the concurrency guarantee.
 */
ELDER_TERMS_API cardio::promise<std::string> ensure_connection_id_async(
    std::filesystem::path path, cardio::cancellation cancellation);

} // namespace elder_terms
