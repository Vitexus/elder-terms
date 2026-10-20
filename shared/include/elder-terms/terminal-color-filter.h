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
ELDER_TERMS_API Oklch srgb_to_oklch(RgbColor color);
/** Reduces chroma at fixed hue and lightness to fit sRGB.
 * @param color Oklch color; lightness is clamped, nonfinite input yields black.
 * @returns Eight-bit sRGB color. */
ELDER_TERMS_API RgbColor oklch_to_srgb(Oklch color);
/** Adjusts Oklch lightness to reach 4.5:1 contrast, reducing chroma only to fit sRGB.
 * @param foreground Original sRGB foreground. @param background Reference background.
 * @returns Hue-preserving contrast correction for all hues and neutrals, or the unchanged foreground. */
ELDER_TERMS_API RgbColor improve_color_visibility(RgbColor foreground, RgbColor background);
/** Recolors a display color using a type-specific redistribution of lost color information.
 * @param color Original unassociated eight-bit sRGB.
 * @param profile Color-vision profile, or none for identity.
 * @param strength Correction amount clamped to zero through one; nonfinite means zero.
 * @returns Recolored sRGB. Exact neutrals and zero-strength colors are unchanged.
 * @remarks Apply contrast correction after this function; strength is not clinical severity. */
ELDER_TERMS_API RgbColor correct_color_vision(RgbColor color, TerminalColorVision profile, double strength);
/** Approximates dichromatic appearance for evaluation, not for display correction.
 * @param color Original unassociated eight-bit sRGB.
 * @param profile Brettel 1997 profile adapted to sRGB, or none for identity.
 * @returns Simulated sRGB with gamut clipping; exact neutrals are preserved.
 * @remarks A simulation is an evaluation aid and does not establish individual usability. */
ELDER_TERMS_API RgbColor simulate_color_vision(RgbColor color, TerminalColorVision profile);
/** Stable application-owned color hook context. */
struct TerminalColorFilterContext {
  std::optional<RgbColor> background; /**< Connection override for the default background. */
  bool visibility_correction = false; /**< Enable Oklch correction after default-color resolution. */
  TerminalColorVision color_vision = TerminalColorVision::none; /**< Independent recoloring profile. */
  double color_vision_strength = 1; /**< Recoloring amount from zero through one. */
};
/** Pure boxrig display-color callback; never changes the terminal or its underlay.
 * @param input Original resolved colors and already-filtered underlay.
 * @param context Borrowed TerminalColorFilterContext, valid throughout registration.
 * @returns Display colors with connection defaults and selected correction. */
ELDER_TERMS_API boxrig_ui_color_filter_result filter_terminal_colors(const boxrig_ui_color_filter_input *input, void *context);
}
