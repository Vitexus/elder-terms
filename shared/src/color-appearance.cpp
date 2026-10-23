// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "color-appearance.h"
#include <elder-terms/terminal-color-filter.h>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace elder_terms {
// CAM16/CAT16 equations, Li et al. (2017), DOI:10.1002/col.22131.
// Checked against Colour's reference example and Material Color Utilities:
// https://colour.readthedocs.io/en/develop/generated/colour.XYZ_to_CAM16.html
// https://github.com/material-foundation/material-color-utilities/tree/main/cpp/cam
using Triple = std::array<double, 3>;
static constexpr double cat16[3][3] = {
    {.401288, .650173, -.051461}, {-.250268, 1.204414, .045854}, {-.002079, .048952, .953127}};
static constexpr double inverse_cat16[3][3] = {
    {1.8620678551, -1.0112546305, .1491867754}, {.3875265432, .6214474419, -.0089739852},
    {-.0158414988, -.0341229380, 1.0499644369}};
static Triple multiply(const double matrix[3][3], Triple vector) {
  Triple result{};
  for (std::size_t row = 0; row < 3; row++)
    for (std::size_t column = 0; column < 3; column++) result[row] += matrix[row][column] * vector[column];
  return result;
}
static double compress_cone(double value, double fl) {
  const auto power = std::pow(fl * std::abs(value) / 100, .42);
  return std::copysign(400 * power / (27.13 + power), value);
}
static double expand_cone(double value, double fl) {
  return std::copysign(100 / fl * std::pow(27.13 * std::abs(value) / (400 - std::abs(value)), 1 / .42), value);
}

Cam16Environment make_cam16_environment(ColorXyz white, double adapting_luminance,
    double background_y, double surround_f, double surround_c, double surround_nc) {
  Cam16Environment result{};
  const auto white_cones = multiply(cat16, {white.x, white.y, white.z});
  const auto adaptation = std::clamp(surround_f * (1 - std::exp((-adapting_luminance - 42) / 92) / 3.6), 0.0, 1.0);
  const auto k4 = std::pow(1 / (5 * adapting_luminance + 1), 4);
  result.fl = k4 * adapting_luminance + .1 * std::pow(1 - k4, 2) * std::cbrt(5 * adapting_luminance);
  result.n = background_y / white.y;
  result.z = 1.48 + std::sqrt(result.n);
  result.nbb = .725 / std::pow(result.n, .2);
  result.c = surround_c;
  result.nc = surround_nc;
  Triple adapted_white{};
  for (std::size_t i = 0; i < 3; i++) {
    result.cone_scale[i] = adaptation * white.y / white_cones[i] + 1 - adaptation;
    adapted_white[i] = compress_cone(white_cones[i] * result.cone_scale[i], result.fl);
  }
  result.aw = (2 * adapted_white[0] + adapted_white[1] + .05 * adapted_white[2]) * result.nbb;
  return result;
}

Cam16Color xyz_to_cam16(ColorXyz color, const Cam16Environment &environment) {
  auto cones = multiply(cat16, {color.x, color.y, color.z});
  for (std::size_t i = 0; i < 3; i++) cones[i] = compress_cone(cones[i] * environment.cone_scale[i], environment.fl);
  const auto a = cones[0] - 12 * cones[1] / 11 + cones[2] / 11;
  const auto b = (cones[0] + cones[1] - 2 * cones[2]) / 9;
  const auto achromatic = (2 * cones[0] + cones[1] + .05 * cones[2]) * environment.nbb;
  if (achromatic <= 0) return {};
  const auto j = 100 * std::pow(achromatic / environment.aw, environment.c * environment.z);
  const auto angle = std::atan2(b, a);
  const auto eccentricity = .25 * (std::cos(angle + 2) + 3.8);
  const auto t = 50000 / 13.0 * environment.nc * environment.nbb * eccentricity * std::hypot(a, b) /
      (cones[0] + cones[1] + 1.05 * cones[2] + .305);
  return {j, std::pow(t, .9) * std::sqrt(j / 100) * std::pow(1.64 - std::pow(.29, environment.n), .73),
      std::fmod(angle * 180 / std::numbers::pi + 360, 360)};
}

ColorXyz cam16_to_xyz(Cam16Color color, const Cam16Environment &environment) {
  if (color.lightness <= 0) return {};
  const auto angle = color.hue * std::numbers::pi / 180;
  const auto cosine = std::cos(angle), sine = std::sin(angle);
  const auto t = std::pow(color.chroma / std::sqrt(color.lightness / 100) /
      std::pow(1.64 - std::pow(.29, environment.n), .73), 1 / .9);
  const auto p1 = 50000 / 13.0 * .25 * (std::cos(angle + 2) + 3.8) * environment.nc * environment.nbb;
  const auto p2 = environment.aw / environment.nbb * std::pow(color.lightness / 100, 1 / (environment.c * environment.z));
  const auto radius = 23 * (p2 + .305) * t / (23 * p1 + t * (11 * cosine + 108 * sine));
  const auto a = radius * cosine, b = radius * sine;
  Triple cones{(460 * p2 + 451 * a + 288 * b) / 1403, (460 * p2 - 891 * a - 261 * b) / 1403,
      (460 * p2 - 220 * a - 6300 * b) / 1403};
  for (std::size_t i = 0; i < 3; i++) cones[i] = expand_cone(cones[i], environment.fl) / environment.cone_scale[i];
  const auto xyz = multiply(inverse_cat16, cones);
  return {xyz[0], xyz[1], xyz[2]};
}

static ColorXyz srgb_xyz(RgbColor color) {
  const auto decode = [](double value) {
    value /= 255;
    return 100 * (value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4));
  };
  const double r = decode(color.red), g = decode(color.green), b = decode(color.blue);
  return {.41233895 * r + .35762064 * g + .18051042 * b, .2126 * r + .7152 * g + .0722 * b,
      .01932141 * r + .11916382 * g + .95034478 * b};
}
static Triple xyz_linear(ColorXyz color) {
  return {(3.241377479 * color.x - 1.537665240 * color.y - .498853668 * color.z) / 100,
      (-.969145252 * color.x + 1.875885346 * color.y + .041565856 * color.z) / 100,
      (.055620937 * color.x - .203955246 * color.y + 1.057179912 * color.z) / 100};
}
static bool in_gamut(Triple color) {
  return *std::min_element(color.begin(), color.end()) >= -1e-7 && *std::max_element(color.begin(), color.end()) <= 1.0000001;
}
static RgbColor encode(Triple color) {
  const auto channel = [](double value) {
    value = std::clamp(value, 0.0, 1.0);
    return static_cast<guint8>(std::lround(255 * (value <= .0031308 ? 12.92 * value : 1.055 * std::pow(value, 1 / 2.4) - .055)));
  };
  return {channel(color[0]), channel(color[1]), channel(color[2])};
}

RgbColor compensate_color_appearance(RgbColor original, RgbColor reversed) {
  // Application assumptions, fixed per mode, never inferred from an individual
  // cell: D65; dark display LA=20 cd/m², Yb=5%, dim surround; light display
  // LA=64 cd/m², Yb=80%, average surround. These are not ambient measurements.
  static const auto dark = make_cam16_environment({95.047, 100, 108.883}, 20, 5, .9, .59, .9);
  static const auto light = make_cam16_environment({95.047, 100, 108.883}, 64, 80, 1, .69, 1);
  auto amount = std::clamp(srgb_to_oklch(original).chroma / .08, 0.0, 1.0);
  amount = amount * amount * (3 - 2 * amount);
  if (amount < 1e-12) return reversed;
  auto appearance = xyz_to_cam16(srgb_xyz(original), dark);
  appearance.lightness = xyz_to_cam16(srgb_xyz(reversed), light).lightness;
  auto color = xyz_linear(cam16_to_xyz(appearance, light));
  if (!in_gamut(color)) {
    double low = 0, high = appearance.chroma;
    for (int iteration = 0; iteration < 20; iteration++) {
      const auto middle = (low + high) / 2;
      const auto candidate = xyz_linear(cam16_to_xyz({appearance.lightness, middle, appearance.hue}, light));
      if (in_gamut(candidate)) low = middle;
      else high = middle;
    }
    color = xyz_linear(cam16_to_xyz({appearance.lightness, low, appearance.hue}, light));
  }
  const auto base = srgb_to_oklch(reversed), compensated = srgb_to_oklch(encode(color));
  const auto radians = std::numbers::pi / 180;
  const auto a = std::lerp(base.chroma * std::cos(base.hue * radians), compensated.chroma * std::cos(compensated.hue * radians), amount);
  const auto b = std::lerp(base.chroma * std::sin(base.hue * radians), compensated.chroma * std::sin(compensated.hue * radians), amount);
  return oklch_to_srgb({std::lerp(base.lightness, compensated.lightness, amount), std::hypot(a, b), std::atan2(b, a) / radians});
}
}
