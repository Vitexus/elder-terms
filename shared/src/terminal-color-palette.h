#pragma once
#include <elder-terms/terminal-color-filter.h>

namespace elder_terms {
// Applies the precomputed continuous displacement field, then checks the
// actual cell background. Palette storage is immutable during rendering.
RgbColor apply_reverse_palette(RgbColor original, RgbColor candidate, RgbColor background, const ReversePalette &palette);
}
