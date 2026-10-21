#pragma once

#include <memory>
#include <string>

#include <cardio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include <elder-terms/export.h>

namespace elder_terms {

/** Result of resolving one connection icon specification. */
struct ConnectionIconLoadResult {
  /** Shared, square 256-pixel RGBA image, including the default fallback.
   * Callers must not mutate the pixels. */
  std::shared_ptr<GdkPixbuf> image;
  /** True when the requested image was unavailable and the default was used. */
  bool fallback = false;
  /** User-visible reason for a fallback; empty for a requested default. */
  std::string error;
};

/**
 * Gets the bundled default connection icon.
 *
 * @returns Shared, owned 256-pixel RGBA image, or null if the bundled image
 * cannot be decoded. Callers must not mutate the shared pixels.
 */
ELDER_TERMS_API std::shared_ptr<GdkPixbuf> default_connection_icon();

/**
 * Resolves a saved connection icon without blocking the caller on file I/O.
 *
 * @param specification Saved `default`, `file:/absolute/path`, `noto:id`, or
 * `material:id` specification.
 * @param cancellation Cancellation signal for file I/O.
 * @returns A 256-pixel square image and fallback diagnosis. Unsupported
 * presets and invalid or unavailable files retain the saved specification but
 * return the default image. Callers must not mutate the shared pixels.
 * @throws cardio::canceled_exception when cancellation is requested.
 *
 * @remarks Requires a running cardio GLib dispatcher. PNG and JPEG file
 * images are accepted; input is limited to 64 MiB encoded data, 64 MiB
 * declared original RGBA size, and 16384 pixels per side.
 */
ELDER_TERMS_API cardio::promise<ConnectionIconLoadResult>
load_connection_icon_async(std::string specification,
                           cardio::cancellation cancellation);

} // namespace elder_terms
