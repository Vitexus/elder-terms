#include "terminal-color-filter.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace elder_terms {
struct LinearRgb { double red, green, blue; };

static double linear_channel(double value) {
  value /= 255;
  return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4);
}
static LinearRgb linear_rgb(RgbColor color) {
  return {linear_channel(color.red), linear_channel(color.green), linear_channel(color.blue)};
}
static double luminance(LinearRgb color) { return .2126 * color.red + .7152 * color.green + .0722 * color.blue; }
static double contrast(double a, double b) { return (std::max(a, b) + .05) / (std::min(a, b) + .05); }

Oklch srgb_to_oklch(RgbColor color) {
  // D65 matrices published by Björn Ottosson (public domain):
  // https://bottosson.github.io/posts/oklab/#converting-from-linear-srgb-to-oklab
  const auto rgb = linear_rgb(color);
  const double l = std::cbrt(.4122214708 * rgb.red + .5363325363 * rgb.green + .0514459929 * rgb.blue);
  const double m = std::cbrt(.2119034982 * rgb.red + .6806995451 * rgb.green + .1073969566 * rgb.blue);
  const double s = std::cbrt(.0883024619 * rgb.red + .2817188376 * rgb.green + .6299787005 * rgb.blue);
  const double a = 1.9779984951 * l - 2.4285922050 * m + .4505937099 * s;
  const double b = .0259040371 * l + .7827717662 * m - .8086757660 * s;
  const double hue = std::atan2(b, a) * 180 / std::numbers::pi;
  return {.2104542553 * l + .7936177850 * m - .0040720468 * s, std::hypot(a, b), hue < 0 ? hue + 360 : hue};
}

static LinearRgb oklab_to_linear(double lightness, double a, double b) {
  const double l0 = lightness + .3963377774 * a + .2158037573 * b;
  const double m0 = lightness - .1055613458 * a - .0638541728 * b;
  const double s0 = lightness - .0894841775 * a - 1.2914855480 * b;
  const double l = l0 * l0 * l0, m = m0 * m0 * m0, s = s0 * s0 * s0;
  return {4.0767416621 * l - 3.3077115913 * m + .2309699292 * s,
      -1.2684380046 * l + 2.6097574011 * m - .3413193965 * s,
      -.0041960863 * l - .7034186147 * m + 1.7076147010 * s};
}

static bool in_gamut(LinearRgb color) {
  // Allow matrix roundoff at exact primary/white boundaries, not visible clipping.
  return std::min({color.red, color.green, color.blue}) >= -1e-7 &&
      std::max({color.red, color.green, color.blue}) <= 1.0000001;
}

static LinearRgb gamut_map(double lightness, double chroma, double cosine, double sine) {
  auto color = oklab_to_linear(lightness, chroma * cosine, chroma * sine);
  if (in_gamut(color)) return color;
  double low = 0, high = chroma;
  for (int iteration = 0; iteration < 16; iteration++) {
    const double middle = (low + high) / 2;
    if (in_gamut(oklab_to_linear(lightness, middle * cosine, middle * sine))) low = middle;
    else high = middle;
  }
  return oklab_to_linear(lightness, low * cosine, low * sine);
}

static RgbColor encode(LinearRgb color) {
  const auto channel = [](double value) {
    value = std::clamp(value, 0.0, 1.0);
    const auto encoded = value <= .0031308 ? value * 12.92 : 1.055 * std::pow(value, 1 / 2.4) - .055;
    return static_cast<guint8>(std::lround(encoded * 255));
  };
  return {channel(color.red), channel(color.green), channel(color.blue)};
}

RgbColor oklch_to_srgb(Oklch color) {
  if (!std::isfinite(color.lightness) || !std::isfinite(color.chroma) || !std::isfinite(color.hue)) return {};
  const auto angle = std::fmod(color.hue, 360) * std::numbers::pi / 180;
  // All sRGB chroma is below one; cap larger inputs before cubing the matrices.
  return encode(gamut_map(std::clamp(color.lightness, 0.0, 1.0), std::clamp(color.chroma, 0.0, 1.0), std::cos(angle), std::sin(angle)));
}

static double smoothstep(double value) {
  value = std::clamp(value, 0.0, 1.0);
  return value * value * (3 - 2 * value);
}

RgbColor improve_blue_visibility(RgbColor foreground, RgbColor background) {
  // Neutral text dominates ordinary shell output and never needs blue correction.
  if (foreground.red == foreground.green && foreground.green == foreground.blue) return foreground;
  const auto bg = luminance(linear_rgb(background));
  const auto original_contrast = contrast(luminance(linear_rgb(foreground)), bg);
  if (original_contrast >= 4.5) return foreground;
  const auto color = srgb_to_oklch(foreground);
  const double weight = smoothstep((color.hue - 230) / 20) * (1 - smoothstep((color.hue - 285) / 20)) *
      smoothstep((color.chroma - .015) / .025);
  if (weight == 0) return foreground;
  const double cosine = std::cos(color.hue * std::numbers::pi / 180), sine = std::sin(color.hue * std::numbers::pi / 180);
  const double direction = contrast(1, bg) >= contrast(0, bg) ? 1 : -1;
  const double endpoint = std::clamp(color.lightness + direction * .42 * weight, 0.0, 1.0);
  auto best = gamut_map(endpoint, color.chroma, cosine, sine);
  if (contrast(luminance(best), bg) >= 4.6) {
    // Search linear-light colors; encode once after convergence. The margin above
    // 4.5 keeps eight-bit rounding from retriggering correction of these samples.
    double low = 0, high = std::abs(endpoint - color.lightness);
    for (int iteration = 0; iteration < 16; iteration++) {
      const double distance = (low + high) / 2;
      const auto candidate = gamut_map(color.lightness + direction * distance, color.chroma, cosine, sine);
      if (contrast(luminance(candidate), bg) >= 4.6) { high = distance; best = candidate; }
      else low = distance;
    }
  }
  const auto corrected = encode(best);
  return contrast(luminance(linear_rgb(corrected)), bg) > original_contrast ? corrected : foreground;
}

boxrig_ui_color_filter_result filter_terminal_colors(const boxrig_ui_color_filter_input *input, void *data) {
  const auto &context = *static_cast<const TerminalColorFilterContext *>(data);
  auto result = boxrig_ui_color_filter_result_default(input);
  if (context.background) {
    const auto background = *context.background;
    const auto y = luminance(linear_rgb(background));
    const guint8 text = contrast(1, y) >= contrast(0, y) ? 255 : 0;
    const auto replace_default = [&](boxrig_rgb value, boxrig_ui_color_source_role role) {
      if (role == BOXRIG_UI_COLOR_DEFAULT_BACKGROUND) return boxrig_rgb{background.red, background.green, background.blue};
      if (role == BOXRIG_UI_COLOR_DEFAULT_FOREGROUND || role == BOXRIG_UI_COLOR_DEFAULT_CURSOR) return boxrig_rgb{text, text, text};
      return value;
    };
    result.foreground = replace_default(result.foreground, input->foreground_source.role);
    result.background = replace_default(result.background, input->background_source.role);
    result.underline = replace_default(result.underline, input->underline_source.role);
    result.marker = replace_default(result.marker, input->marker_source.role);
  }
  if (!context.blue_visibility) return result;
  const auto correct = [](boxrig_rgb foreground, boxrig_rgb background) {
    const auto color = improve_blue_visibility({foreground.red, foreground.green, foreground.blue},
        {background.red, background.green, background.blue});
    return boxrig_rgb{color.red, color.green, color.blue};
  };
  if (input->target == BOXRIG_UI_COLOR_CELL || input->target == BOXRIG_UI_COLOR_IME) {
    result.foreground = correct(result.foreground, result.background);
    if (!result.underline_follows_foreground) result.underline = correct(result.underline, result.background);
  } else if (input->target != BOXRIG_UI_COLOR_SURFACE) {
    // The underlay has already passed through this hook. Use it only as a
    // contrast reference; do not apply defaults or correction to it again.
    result.marker = correct(result.marker, input->underlay_valid ? input->underlay.background : result.background);
  }
  return result;
}
}
