#include "terminal-color-palette.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace elder_terms {
struct PaletteLab { double lightness, a, b; };
struct ReversePalette {
  const boxrig_terminal *terminal;
  uint64_t revision;
  uint32_t background_key;
  TerminalColorVision vision;
  double strength;
  // Seventeen knots per encoded RGB axis; interpolate displacements rather
  // than colors so the role-based tone curve and neutral axis remain intact.
  std::array<PaletteLab, 17 * 17 * 17> offsets{};
};
static uint32_t rgb_key(RgbColor color) { return (uint32_t{color.red} << 16) | (uint32_t{color.green} << 8) | color.blue; }
static PaletteLab lab(RgbColor color) {
  const auto lch = srgb_to_oklch(color);
  const auto angle = lch.hue * std::numbers::pi / 180;
  return {lch.lightness, lch.chroma * std::cos(angle), lch.chroma * std::sin(angle)};
}
static double distance_squared(PaletteLab a, PaletteLab b) {
  return std::pow(a.lightness - b.lightness, 2) + std::pow(a.a - b.a, 2) + std::pow(a.b - b.b, 2);
}
static RgbColor rgb(PaletteLab color) {
  return oklch_to_srgb({color.lightness, std::hypot(color.a, color.b), std::atan2(color.b, color.a) * 180 / std::numbers::pi});
}
static RgbColor role_foreground(RgbColor foreground, RgbColor background) {
  TerminalColorFilterContext context{std::nullopt, TerminalColorFilter::reverse_roles};
  boxrig_ui_color_filter_input input{};
  input.target = BOXRIG_UI_COLOR_CELL;
  input.colors.foreground = {foreground.red, foreground.green, foreground.blue};
  input.colors.background = {background.red, background.green, background.blue};
  input.foreground_source.role = input.background_source.role = BOXRIG_UI_COLOR_RGB;
  const auto result = filter_terminal_colors(&input, &context).foreground;
  return {result.red, result.green, result.blue};
}
static RgbColor role_background(RgbColor background) {
  TerminalColorFilterContext context{background, TerminalColorFilter::reverse_roles};
  boxrig_ui_color_filter_input input{};
  input.target = BOXRIG_UI_COLOR_SURFACE;
  input.background_source.role = BOXRIG_UI_COLOR_DEFAULT_BACKGROUND;
  const auto result = filter_terminal_colors(&input, &context).background;
  return {result.red, result.green, result.blue};
}

static void optimize_palette(ReversePalette &result, const std::vector<RgbColor> &colors, RgbColor background) {
  const auto displayed_background = role_background(background);
  const auto source_background = lab(background), target_background = lab(displayed_background);
  std::vector<PaletteLab> source, initial, chosen;
  for (const auto color : colors) {
    source.push_back(lab(color));
    initial.push_back(lab(role_foreground(color, background)));
  }
  chosen = initial;
  const auto size = colors.size();
  std::vector<double> separations(size * size), background_distances(size);
  for (std::size_t i = 0; i < size; i++) {
    background_distances[i] = std::sqrt(distance_squared(source[i], source_background));
    for (std::size_t j = 0; j < size; j++) separations[i * size + j] = std::sqrt(distance_squared(source[i], source[j]));
  }
  // Application adaptation of Chameleon's joint color/contrast/separation
  // objective (https://arxiv.org/html/2512.00516v2). Use all palette pairs in
  // Oklab because terminal adjacency changes when scrolling. A deterministic
  // coordinate descent replaces stochastic image-based annealing. The 4.5:1
  // constraint is enforced on every candidate after gamut mapping/rounding.
  const auto loss = [&](std::size_t index, PaletteLab candidate) {
    double value = .5 * distance_squared(candidate, initial[index]);
    value += .15 * std::pow(std::sqrt(distance_squared(candidate, target_background)) - background_distances[index], 2);
    for (std::size_t j = 0; j < size; j++) if (index != j)
      value += 2.0 / size * std::pow(std::sqrt(distance_squared(candidate, chosen[j])) - separations[index * size + j], 2);
    return value;
  };
  for (int pass = 0; pass < 8; pass++) {
    const auto step = .03 * std::pow(.75, pass);
    for (std::size_t i = 0; i < size; i++) {
      const auto original = srgb_to_oklch(colors[i]);
      if (original.chroma < .01) continue;
      auto best = chosen[i];
      auto best_loss = loss(i, best);
      const auto current = srgb_to_oklch(rgb(best));
      const std::array<Oklch, 6> candidates{{
          {current.lightness - step, current.chroma, current.hue}, {current.lightness + step, current.chroma, current.hue},
          {current.lightness, current.chroma - step / 2, current.hue}, {current.lightness, current.chroma + step / 2, current.hue},
          {current.lightness, current.chroma, current.hue - step * 100}, {current.lightness, current.chroma, current.hue + step * 100}}};
      for (auto candidate : candidates) {
        if (std::abs(std::remainder(candidate.hue - original.hue, 360)) > 12) continue;
        candidate.chroma = std::clamp(candidate.chroma, 0.0, original.chroma);
        const auto mapped = lab(improve_color_visibility(oklch_to_srgb(candidate), displayed_background));
        const auto candidate_loss = loss(i, mapped);
        if (candidate_loss < best_loss) { best = mapped; best_loss = candidate_loss; }
      }
      chosen[i] = best;
    }
  }
  // Shepard interpolation of optimized-minus-initial displacements. Duplicate
  // palette colors were removed, so equal RGB always has one mapping regardless
  // of INDEXED versus RGB origin. The lookup grid keeps frame costs bounded.
  for (int r = 0; r < 17; r++) for (int g = 0; g < 17; g++) for (int b = 0; b < 17; b++) {
    const auto point = lab({static_cast<guint8>(std::lround(r * 255.0 / 16)),
        static_cast<guint8>(std::lround(g * 255.0 / 16)), static_cast<guint8>(std::lround(b * 255.0 / 16))});
    PaletteLab offset{};
    double total = 0;
    for (std::size_t i = 0; i < size; i++) {
      const auto squared = distance_squared(point, source[i]);
      const auto weight = 1 / std::pow(std::max(1e-12, squared), 2);
      offset.lightness += weight * (chosen[i].lightness - initial[i].lightness);
      offset.a += weight * (chosen[i].a - initial[i].a);
      offset.b += weight * (chosen[i].b - initial[i].b);
      total += weight;
    }
    result.offsets[(r * 17 + g) * 17 + b] = {offset.lightness / total, offset.a / total, offset.b / total};
  }
}

bool prepare_terminal_color_filter(TerminalColorFilterContext *context, const boxrig_terminal *terminal) {
  if (context->filter != TerminalColorFilter::reverse_palette) { context->reverse_palette.reset(); return true; }
  const auto revision = terminal ? boxrig_terminal_color_revision(terminal) : 0;
  const auto background_key = context->background ? rgb_key(*context->background) : 0x1000000;
  const auto strength = std::isfinite(context->color_vision_strength) ? std::clamp(context->color_vision_strength, 0.0, 1.0) : 0;
  const auto &previous = context->reverse_palette;
  if (previous && previous->terminal == terminal && previous->revision == revision && previous->background_key == background_key &&
      previous->vision == context->color_vision && previous->strength == strength) return true;
  std::unique_ptr<boxrig_terminal, decltype(&boxrig_terminal_destroy)> standard(nullptr, boxrig_terminal_destroy);
  auto *source = terminal;
  if (!source) {
    auto config = boxrig_terminal_config_default();
    config.rows = 1; config.columns = 1;
    boxrig_terminal *created = nullptr;
    if (boxrig_terminal_create(&config, &created) != BOXRIG_STATUS_OK) return false;
    standard.reset(created);
    source = created;
  }
  std::vector<RgbColor> colors;
  for (int index = 0; index < 258; index++) {
    boxrig_rgb color{};
    const auto status = index < 256 ? boxrig_terminal_get_palette_color(source, static_cast<uint8_t>(index), &color) :
        boxrig_terminal_get_dynamic_color(source, index == 256 ? BOXRIG_DYNAMIC_COLOR_FOREGROUND : BOXRIG_DYNAMIC_COLOR_CURSOR, &color);
    if (status != BOXRIG_STATUS_OK) return false;
    colors.push_back(correct_color_vision({color.red, color.green, color.blue}, context->color_vision, strength));
  }
  std::sort(colors.begin(), colors.end(), [](RgbColor a, RgbColor b) { return rgb_key(a) < rgb_key(b); });
  colors.erase(std::unique(colors.begin(), colors.end(), [](RgbColor a, RgbColor b) { return rgb_key(a) == rgb_key(b); }), colors.end());
  boxrig_rgb default_background{};
  if (boxrig_terminal_get_dynamic_color(source, BOXRIG_DYNAMIC_COLOR_BACKGROUND, &default_background) != BOXRIG_STATUS_OK) return false;
  const auto background = context->background.value_or(RgbColor{default_background.red, default_background.green, default_background.blue});
  auto prepared = std::make_shared<ReversePalette>();
  prepared->terminal = terminal;
  prepared->revision = revision;
  prepared->background_key = background_key;
  prepared->vision = context->color_vision;
  prepared->strength = strength;
  optimize_palette(*prepared, colors, background);
  context->reverse_palette = std::move(prepared);
  return true;
}

RgbColor apply_reverse_palette(RgbColor original, RgbColor candidate, RgbColor background, const ReversePalette &palette) {
  const double coordinates[] = {original.red * 16.0 / 255, original.green * 16.0 / 255, original.blue * 16.0 / 255};
  int lower[3];
  double fraction[3];
  for (int i = 0; i < 3; i++) {
    lower[i] = std::min(15, static_cast<int>(coordinates[i]));
    fraction[i] = coordinates[i] - lower[i];
  }
  PaletteLab delta{};
  for (int r = 0; r < 2; r++) for (int g = 0; g < 2; g++) for (int b = 0; b < 2; b++) {
    const auto weight = (r ? fraction[0] : 1 - fraction[0]) * (g ? fraction[1] : 1 - fraction[1]) * (b ? fraction[2] : 1 - fraction[2]);
    const auto &offset = palette.offsets[((lower[0] + r) * 17 + lower[1] + g) * 17 + lower[2] + b];
    delta.lightness += weight * offset.lightness;
    delta.a += weight * offset.a;
    delta.b += weight * offset.b;
  }
  auto amount = std::clamp(srgb_to_oklch(original).chroma / .08, 0.0, 1.0);
  amount = amount * amount * (3 - 2 * amount);
  const auto base = lab(candidate);
  return improve_color_visibility(rgb({base.lightness + amount * delta.lightness, base.a + amount * delta.a, base.b + amount * delta.b}), background);
}
}
