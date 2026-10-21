#include <elder-terms/terminal-color-filter.h>
#include <boxrig/ui.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace elder_terms;

static bool equal(boxrig_rgb a, boxrig_rgb b) {
  return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

static boxrig_ui_color_filter_result cell(TerminalColorFilter filter, boxrig_rgb foreground,
    boxrig_rgb background, uint32_t attributes = 0) {
  TerminalColorFilterContext context{std::nullopt, filter};
  boxrig_ui_color_filter_input input{};
  input.target = BOXRIG_UI_COLOR_CELL;
  input.colors = {foreground, background, foreground, {}, true};
  input.foreground_source.role = input.background_source.role = BOXRIG_UI_COLOR_RGB;
  input.attributes = attributes;
  return filter_terminal_colors(&input, &context);
}

static void test_lightness() {
  constexpr auto filter = TerminalColorFilter::reverse_lightness;
  const auto normal = cell(filter, {255, 255, 255}, {0, 0, 0});
  assert(equal(normal.foreground, {0, 0, 0}));
  assert(equal(normal.background, {255, 255, 255}));
  const auto inverse = cell(filter, {0, 0, 0}, {255, 255, 255}, BOXRIG_CELL_INVERSE);
  assert(equal(inverse.foreground, normal.background));
  assert(equal(inverse.background, normal.foreground));
  // Independently calculated reference samples from the Oklab D65 matrices.
  const auto red = cell(filter, {255, 0, 0}, {32, 32, 32});
  assert(equal(red.foreground, {126, 0, 0}));
  assert(equal(red.background, {176, 176, 176}));
  const auto near = cell(filter, {254, 253, 254}, {1, 1, 1});
  assert(near.foreground.red < 3 && near.foreground.green < 3 && near.foreground.blue < 3);
  assert(equal(near.background, {233, 233, 233}));
  for (int value = 0; value < 255; value++) {
    const auto v = static_cast<uint8_t>(value), next = static_cast<uint8_t>(value + 1);
    const auto pair = cell(filter, {v, v, v}, {next, next, next});
    assert(pair.foreground.red >= pair.background.red);
    assert(pair.foreground.red == pair.foreground.green && pair.foreground.green == pair.foreground.blue);
  }
  for (const auto target : {BOXRIG_UI_COLOR_SURFACE, BOXRIG_UI_COLOR_IME, BOXRIG_UI_COLOR_CURSOR,
      BOXRIG_UI_COLOR_SELECTION_MARKER, BOXRIG_UI_COLOR_SEARCH_MARKER, BOXRIG_UI_COLOR_HOVER_MARKER, BOXRIG_UI_COLOR_IME_MARKER}) {
    TerminalColorFilterContext context{std::nullopt, filter};
    boxrig_ui_color_filter_input input{};
    input.target = target;
    input.colors = {{255, 255, 255}, {0, 0, 0}, {255, 255, 255}, {255, 255, 255}, false};
    input.underlay_valid = true;
    input.underlay.background = {255, 255, 255};
    const auto result = filter_terminal_colors(&input, &context);
    if (target == BOXRIG_UI_COLOR_SURFACE || target == BOXRIG_UI_COLOR_IME) {
      assert(equal(result.background, {255, 255, 255}));
      if (target == BOXRIG_UI_COLOR_IME) assert(equal(result.underline, {0, 0, 0}));
    } else assert(equal(result.marker, {0, 0, 0}));
    assert(equal(input.underlay.background, {255, 255, 255}));
  }
}

static double luminance(boxrig_rgb rgb) {
  const auto channel = [](double value) {
    value /= 255;
    return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4);
  };
  return .2126 * channel(rgb.red) + .7152 * channel(rgb.green) + .0722 * channel(rgb.blue);
}

static double contrast(boxrig_rgb a, boxrig_rgb b) {
  const double x = luminance(a) + .05, y = luminance(b) + .05;
  return std::max(x, y) / std::min(x, y);
}

static void test_contrast() {
  constexpr auto filter = TerminalColorFilter::reverse_contrast;
  const auto reversed = cell(filter, {255, 255, 255}, {0, 0, 0});
  assert(equal(reversed.foreground, {0, 0, 0}) && equal(reversed.background, {255, 255, 255}));
  for (int r = 0; r <= 255; r += 17) for (int g = 0; g <= 255; g += 17) for (int b = 0; b <= 255; b += 17) {
    const boxrig_rgb source{static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)};
    const auto transformed = cell(filter, source, {53, 82, 113});
    const auto y = luminance(source);
    const auto expected = (1 - y) / (1 + 20 * y);
    assert(std::abs(luminance(transformed.foreground) - expected) < .0045);
    const auto original = contrast(source, {53, 82, 113});
    assert(std::abs(contrast(transformed.foreground, transformed.background) / original - 1) < .025);
    const auto again = cell(filter, transformed.foreground, {0, 0, 0});
    assert(std::abs(luminance(again.foreground) - y) < .025);
  }
  // B preserves poor contrast too; it must not silently become Oklch or C.
  const auto poor = cell(filter, {128, 128, 128}, {120, 120, 120});
  assert(contrast(poor.foreground, poor.background) < 1.2);
}

static void test_roles() {
  constexpr auto filter = TerminalColorFilter::reverse_roles;
  const auto normal = cell(filter, {255, 255, 255}, {0, 0, 0});
  assert(equal(normal.foreground, {0, 0, 0}) && equal(normal.background, {255, 255, 255}));
  const auto near = cell(filter, {252, 253, 252}, {2, 1, 2});
  assert(std::max({near.foreground.red, near.foreground.green, near.foreground.blue}) < 10);
  assert(std::min({near.background.red, near.background.green, near.background.blue}) > 245);
  const auto dark = cell(filter, {128, 128, 0}, {0, 0, 0});
  const auto bright = cell(filter, {255, 255, 0}, {0, 0, 0});
  assert(!equal(dark.foreground, bright.foreground));
  assert(luminance(bright.foreground) > luminance(dark.foreground));
  // Hue should still say yellow, even though it must be darker on white.
  assert(bright.foreground.red > 80 && bright.foreground.green > 80 && bright.foreground.blue < 30);
  for (const boxrig_rgb background : {boxrig_rgb{0, 0, 0}, boxrig_rgb{32, 32, 32}, boxrig_rgb{255, 255, 255},
      boxrig_rgb{90, 20, 110}, boxrig_rgb{128, 128, 128}}) {
    for (int r = 0; r <= 255; r += 51) for (int g = 0; g <= 255; g += 51) for (int b = 0; b <= 255; b += 51) {
      const boxrig_rgb source{static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)};
      const auto result = cell(filter, source, background);
      assert(contrast(result.foreground, result.background) >= 4.5);
      assert(equal(result.foreground, cell(filter, source, background, BOXRIG_CELL_INVERSE).foreground));
    }
  }
  for (const auto target : {BOXRIG_UI_COLOR_CURSOR, BOXRIG_UI_COLOR_SELECTION_MARKER, BOXRIG_UI_COLOR_SEARCH_MARKER,
      BOXRIG_UI_COLOR_HOVER_MARKER, BOXRIG_UI_COLOR_IME_MARKER}) {
    TerminalColorFilterContext context{std::nullopt, filter};
    boxrig_ui_color_filter_input input{};
    input.target = target;
    input.colors.marker = {255, 255, 0};
    input.underlay_valid = true;
    input.underlay.background = {240, 240, 240};
    const auto result = filter_terminal_colors(&input, &context);
    assert(contrast(result.marker, input.underlay.background) >= 4.5);
    assert(equal(input.underlay.background, {240, 240, 240}));
  }
}

int main() {
  test_lightness();
  test_contrast();
  test_roles();
  std::puts("reverse color filters passed");
}
