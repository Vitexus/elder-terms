#include <elder-terms/terminal-color-filter.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace elder_terms;

static bool equal(RgbColor a, RgbColor b) {
  return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

static double distance(RgbColor a, RgbColor b, TerminalColorVision profile) {
  const auto x = srgb_to_oklch(simulate_color_vision(a, profile));
  const auto y = srgb_to_oklch(simulate_color_vision(b, profile));
  const auto radians = std::acos(-1) / 180;
  return std::hypot(x.lightness - y.lightness,
      x.chroma * std::cos(x.hue * radians) - y.chroma * std::cos(y.hue * radians),
      x.chroma * std::sin(x.hue * radians) - y.chroma * std::sin(y.hue * radians));
}

int main() {
  const std::array profiles{TerminalColorVision::protan, TerminalColorVision::deutan, TerminalColorVision::tritan};
  // Independently obtained by running the unmodified public-domain reference:
  // https://github.com/DaltonLens/libDaltonLens/blob/1a01fc1bf8d8dd419af8343b80b05e98ba50a75d/libDaltonLens.c
  // Its byte conversion truncates some values; allow one byte of rounding difference.
  const std::array<RgbColor, 4> source{{{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {120, 80, 210}}};
  const RgbColor reference[3][4] = {
    {{107, 92, 12}, {255, 237, 0}, {0, 55, 255}, {0, 94, 210}},
    {{164, 139, 0}, {241, 208, 46}, {0, 86, 254}, {0, 111, 209}},
    {{255, 0, 77}, {120, 233, 255}, {0, 98, 136}, {93, 110, 117}},
  };
  for (std::size_t type = 0; type < profiles.size(); type++) {
    for (std::size_t index = 0; index < source.size(); index++) {
      const auto actual = simulate_color_vision(source[index], profiles[type]);
      const auto expected = reference[type][index];
      assert(std::abs(actual.red - expected.red) <= 1);
      assert(std::abs(actual.green - expected.green) <= 1);
      assert(std::abs(actual.blue - expected.blue) <= 1);
    }
  }

  // These are ordinary ANSI red/green and green/cyan pairs, fixed before the
  // implementation. Compare the final displayed colors, including Oklch.
  const std::array red_green{std::array{RgbColor{128, 0, 0}, RgbColor{0, 128, 0}},
      std::array{RgbColor{255, 0, 0}, RgbColor{0, 255, 0}}};
  const std::array green_cyan{std::array{RgbColor{0, 128, 0}, RgbColor{0, 128, 128}},
      std::array{RgbColor{0, 255, 0}, RgbColor{0, 255, 255}}};
  for (const auto profile : profiles) {
    for (const auto &pair : profile == TerminalColorVision::tritan ? green_cyan : red_green) {
      for (const RgbColor background : {RgbColor{0, 0, 0}, RgbColor{32, 32, 32}, RgbColor{255, 255, 255}}) {
        for (const bool contrast_enabled : {false, true}) {
          const auto displayed = [&](RgbColor color) {
            return contrast_enabled ? improve_color_visibility(color, background) : color;
          };
          const auto before = distance(displayed(pair[0]), displayed(pair[1]), profile);
          const auto after = distance(displayed(correct_color_vision(pair[0], profile, 1)),
              displayed(correct_color_vision(pair[1], profile, 1)), profile);
          std::printf("profile=%d bg=%u oklch=%d distance=%.5f->%.5f\n",
              static_cast<int>(profile), background.red, contrast_enabled, before, after);
          std::fflush(stdout);
          // Product regression criterion in simulated Oklab, not a perceptual
          // threshold for people: gain at least .02 and retain a distance of .04.
          assert(after >= before + .02);
          assert(after >= .04);
        }
      }
    }
    for (int gray = 0; gray <= 255; gray++) {
      const auto channel = static_cast<guint8>(gray);
      const RgbColor neutral{channel, channel, channel};
      assert(equal(simulate_color_vision(neutral, profile), neutral));
      for (const double strength : {0.0, .25, .5, .75, 1.0})
        assert(equal(correct_color_vision(neutral, profile, strength), neutral));
    }
    for (int r = 0; r <= 255; r += 17) for (int g = 0; g <= 255; g += 17) for (int b = 0; b <= 255; b += 17) {
      const RgbColor color{static_cast<guint8>(r), static_cast<guint8>(g), static_cast<guint8>(b)};
      assert(equal(correct_color_vision(color, TerminalColorVision::none, 1), color));
      assert(equal(correct_color_vision(color, profile, 0), color));
      assert(equal(correct_color_vision(color, profile, -1), color));
      assert(equal(correct_color_vision(color, profile, std::numeric_limits<double>::quiet_NaN()), color));
      assert(equal(correct_color_vision(color, profile, std::numeric_limits<double>::infinity()), color));
      const auto full = correct_color_vision(color, profile, 1);
      const auto half = correct_color_vision(color, profile, .5);
      assert(equal(full, correct_color_vision(color, profile, 2)));
      assert(equal(half, correct_color_vision(color, profile, .5)));
      assert(half.red >= std::min(color.red, full.red) && half.red <= std::max(color.red, full.red));
      assert(half.green >= std::min(color.green, full.green) && half.green <= std::max(color.green, full.green));
      assert(half.blue >= std::min(color.blue, full.blue) && half.blue <= std::max(color.blue, full.blue));
    }
  }
  const RgbColor sample{180, 90, 40};
  assert(!equal(correct_color_vision(sample, TerminalColorVision::protan, 1),
      correct_color_vision(sample, TerminalColorVision::deutan, 1)));
  assert(!equal(correct_color_vision(sample, TerminalColorVision::deutan, 1),
      correct_color_vision(sample, TerminalColorVision::tritan, 1)));
  // Full error transfer clips bright red onto magenta and green onto cyan in
  // the protan profile. Keep each basic palette color distinct at maximum gain.
  const std::array<RgbColor, 16> palette{{{0, 0, 0}, {128, 0, 0}, {0, 128, 0}, {128, 128, 0},
      {0, 0, 128}, {128, 0, 128}, {0, 128, 128}, {192, 192, 192}, {128, 128, 128},
      {255, 0, 0}, {0, 255, 0}, {255, 255, 0}, {0, 0, 255}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255}}};
  for (const auto profile : profiles) {
    for (std::size_t first = 0; first < palette.size(); first++)
      for (std::size_t second = first + 1; second < palette.size(); second++)
        assert(!equal(correct_color_vision(palette[first], profile, 1), correct_color_vision(palette[second], profile, 1)));
  }
  std::puts("PASS reference simulation, final-color discrimination, strength, neutrals and repeatability");
}
