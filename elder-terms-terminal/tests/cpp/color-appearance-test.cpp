// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../../shared/src/color-appearance.h"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace elder_terms;

int main() {
  // Independent reference from Colour's CAM16 documentation (Li et al. 2017).
  // https://colour.readthedocs.io/en/develop/generated/colour.XYZ_to_CAM16.html
  const auto environment = make_cam16_environment({95.05, 100, 108.88}, 318.31, 20, 1, .69, 1);
  const auto reference = xyz_to_cam16({19.01, 20, 21.78}, environment);
  assert(std::abs(reference.lightness - 41.7312079) < 1e-6);
  assert(std::abs(reference.chroma - .1033557) < 1e-6);
  assert(std::abs(reference.hue - 217.0679597) < 1e-6);
  for (const auto &viewing : {environment, make_cam16_environment({95.047, 100, 108.883}, 20, 5, .9, .59, .9),
      make_cam16_environment({95.047, 100, 108.883}, 64, 80, 1, .69, 1)}) {
    for (const ColorXyz color : {ColorXyz{0, 0, 0}, ColorXyz{95.047, 100, 108.883},
        ColorXyz{41.233895, 21.26, 1.932141}, ColorXyz{35.762064, 71.52, 11.916382},
        ColorXyz{18.051042, 7.22, 95.034478}, ColorXyz{19.01, 20, 21.78}}) {
      const auto roundtrip = cam16_to_xyz(xyz_to_cam16(color, viewing), viewing);
      assert(std::abs(roundtrip.x - color.x) < 1e-5);
      assert(std::abs(roundtrip.y - color.y) < 1e-5);
      assert(std::abs(roundtrip.z - color.z) < 1e-5);
    }
  }
  std::puts("CAM16 references and viewing-condition roundtrips passed");
}
