// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <gio/gio.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace elder_terms {
/** Immutable decoded image retained for native reattachment and rollback. */
struct TerminalBackgroundImage {
  std::string path;             /**< Absolute source filename in UTF-8. */
  std::size_t width;            /**< Pixel width, at most 16384. */
  std::size_t height;           /**< Pixel height, at most 16384. */
  std::vector<uint8_t> rgba;    /**< Tightly packed, unassociated sRGB RGBA8, at most 64 MiB. */
};
/** Shared immutable image; null means no image. */
using TerminalBackgroundImagePtr = std::shared_ptr<const TerminalBackgroundImage>;
/** Image decoder dependency with the same ownership contract as load_terminal_background_image(). */
using TerminalBackgroundImageLoader = std::function<void(std::string, GCancellable *,
    std::function<void(TerminalBackgroundImagePtr, std::string)>)>;
/**
 * Reads a local PNG/JPEG asynchronously on the calling GLib context.
 * @param path Absolute UTF-8 filename, or empty to clear the image.
 * @param cancellation Optional borrowed cancellation object, retained through completion.
 * @param complete Receives an owned image and empty error, or null and a translated error.
 * @remarks Completion occurs exactly once; an empty path completes synchronously.
 * The loader owns pending I/O and bounds encoded/decoded data to 64 MiB each.
 */
void load_terminal_background_image(std::string path, GCancellable *cancellation,
    std::function<void(TerminalBackgroundImagePtr, std::string)> complete);
}
