#pragma once
#include <boxrig/ui/color.h>
#include <elder-terms/settings/terminal-settings.h>

namespace elder_terms {
/** Perceptual color in D65 Oklch, used for display correction. */
struct Oklch {
  double lightness; /**< Lightness, normally zero through one. */
  double chroma;    /**< Nonnegative chroma. */
  double hue;       /**< Hue angle in degrees, zero through 360. */
};
/** @param color Unassociated eight-bit sRGB. @returns D65 Oklch color. */
Oklch srgb_to_oklch(RgbColor color);
/** Reduces chroma at fixed hue and lightness to fit sRGB.
 * @param color Oklch color; lightness is clamped, nonfinite input yields black.
 * @returns Eight-bit sRGB color. */
RgbColor oklch_to_srgb(Oklch color);
/** @param foreground Original sRGB foreground. @param background Reference background.
 * @returns Hue-preserving blue correction, or the unchanged foreground. */
RgbColor improve_blue_visibility(RgbColor foreground, RgbColor background);
/** Stable application-owned color hook context. */
struct TerminalColorFilterContext {
  std::optional<RgbColor> background; /**< Connection override for the default background. */
  bool blue_visibility = false;      /**< Enable Oklch correction after default-color resolution. */
};
/** Pure boxrig display-color callback; never changes the terminal or its underlay.
 * @param input Original resolved colors and already-filtered underlay.
 * @param context Borrowed TerminalColorFilterContext, valid throughout registration.
 * @returns Display colors with connection defaults and selected correction. */
boxrig_ui_color_filter_result filter_terminal_colors(const boxrig_ui_color_filter_input *input, void *context);
}
