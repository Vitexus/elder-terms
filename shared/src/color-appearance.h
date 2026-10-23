// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once
#include <elder-terms/settings/general-settings.h>
#include <array>

namespace elder_terms {
// Internal CAM16 math uses XYZ on the 0..100 scale and hue in degrees.
struct ColorXyz { double x, y, z; };
struct Cam16Color { double lightness, chroma, hue; };
struct Cam16Environment {
  std::array<double, 3> cone_scale;
  double fl, n, z, nbb, aw, c, nc;
};
Cam16Environment make_cam16_environment(ColorXyz white, double adapting_luminance,
    double background_y, double surround_f, double surround_c, double surround_nc);
Cam16Color xyz_to_cam16(ColorXyz color, const Cam16Environment &environment);
ColorXyz cam16_to_xyz(Cam16Color color, const Cam16Environment &environment);
RgbColor compensate_color_appearance(RgbColor original, RgbColor reversed);
}
