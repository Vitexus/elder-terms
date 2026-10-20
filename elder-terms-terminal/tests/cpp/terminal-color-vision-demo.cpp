#include <elder-terms/terminal-color-filter.h>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <string_view>

using namespace elder_terms;

struct Sample {
  const char *name;
  RgbColor color;
};

static constexpr std::array<Sample, 20> samples{{
    {"BLACK", {0, 0, 0}}, {"- removed", {128, 0, 0}}, {"+ added", {0, 128, 0}}, {"WARN", {128, 128, 0}},
    {"BLUE", {0, 0, 128}}, {"MAGENTA", {128, 0, 128}}, {"CYAN", {0, 128, 128}}, {"GRAY", {192, 192, 192}},
    {"DARK GRAY", {128, 128, 128}}, {"ERROR", {255, 0, 0}}, {"SUCCESS", {0, 255, 0}}, {"YELLOW", {255, 255, 0}},
    {"LIGHT BLUE", {0, 0, 255}}, {"PINK", {255, 0, 255}}, {"LIGHT CYAN", {0, 255, 255}}, {"WHITE", {255, 255, 255}},
    {"256 #166", {215, 95, 0}}, {"256 #37", {0, 175, 175}}, {"RGB ERROR", {200, 80, 80}}, {"RGB INFO", {80, 128, 80}},
}};

static double distance(RgbColor first, RgbColor second, TerminalColorVision profile) {
  const auto a = srgb_to_oklch(simulate_color_vision(first, profile));
  const auto b = srgb_to_oklch(simulate_color_vision(second, profile));
  const auto radians = std::acos(-1) / 180;
  return std::hypot(a.lightness - b.lightness,
      a.chroma * std::cos(a.hue * radians) - b.chroma * std::cos(b.hue * radians),
      a.chroma * std::sin(a.hue * radians) - b.chroma * std::sin(b.hue * radians));
}

static void swatch(RgbColor foreground, RgbColor background, const char *label) {
  std::printf("\033[38;2;%u;%u;%um\033[48;2;%u;%u;%um %-14s ███ \033[0m",
      foreground.red, foreground.green, foreground.blue, background.red, background.green, background.blue, label);
}

static void report() {
  const char *names[] = {"protan", "deutan", "tritan"};
  std::puts("profile\tbackground\toklch\tpair\tbefore\tafter\tgain");
  for (int type = 0; type < 3; type++) {
    const auto profile = static_cast<TerminalColorVision>(type + 1);
    for (const RgbColor background : {RgbColor{0, 0, 0}, RgbColor{32, 32, 32}, RgbColor{255, 255, 255}}) {
      for (const bool contrast_enabled : {false, true}) {
        const auto display = [&](RgbColor color, bool corrected) {
          if (corrected) color = correct_color_vision(color, profile, 1);
          return contrast_enabled ? improve_color_visibility(color, background) : color;
        };
        // Report both intended improvements and all-pair regressions. A filter
        // cannot preserve every distinction when dimensions of vision are lost.
        for (const auto pair : {std::array{1, 2}, std::array{9, 10}, std::array{2, 6},
            std::array{10, 14}, std::array{4, 5}, std::array{12, 13}, std::array{11, 15}, std::array{18, 19}}) {
          const auto before = distance(display(samples[pair[0]].color, false), display(samples[pair[1]].color, false), profile);
          const auto after = distance(display(samples[pair[0]].color, true), display(samples[pair[1]].color, true), profile);
          std::printf("%s\t#%02x%02x%02x\t%d\t%d/%d\t%.5f\t%.5f\t%+.5f\n", names[type],
              background.red, background.green, background.blue, contrast_enabled, pair[0], pair[1], before, after, after - before);
        }
        int improved = 0, reduced = 0;
        double worst = 0;
        int worst_first = 0, worst_second = 0;
        for (int i = 0; i < 16; i++) for (int j = i + 1; j < 16; j++) {
          const auto before = distance(display(samples[i].color, false), display(samples[j].color, false), profile);
          const auto after = distance(display(samples[i].color, true), display(samples[j].color, true), profile);
          const auto gain = after - before;
          if (gain > .005) improved++;
          if (gain < -.005) reduced++;
          if (gain < worst) { worst = gain; worst_first = i; worst_second = j; }
        }
        std::printf("# %s bg=#%02x%02x%02x oklch=%d ANSI pairs=120 improved=%d reduced=%d worst=%d/%d (%+.5f)\n",
            names[type], background.red, background.green, background.blue, contrast_enabled,
            improved, reduced, worst_first, worst_second, worst);
      }
    }
  }
}

int main(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--report") { report(); return 0; }
  const std::string_view name = argc > 1 ? argv[1] : "deutan";
  const auto profile = name == "protan" ? TerminalColorVision::protan : name == "deutan" ? TerminalColorVision::deutan
      : name == "tritan" ? TerminalColorVision::tritan : TerminalColorVision::none;
  int strength = 100;
  bool valid_strength = true;
  if (argc > 2) {
    const std::string_view text = argv[2];
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), strength);
    valid_strength = parsed.ec == std::errc() && parsed.ptr == text.data() + text.size() && strength >= 0 && strength <= 100;
  }
  const auto background = parse_rgb_color(argc > 3 ? argv[3] : "#202020");
  const std::string_view contrast = argc > 4 ? argv[4] : "on";
  if (argc > 5 || profile == TerminalColorVision::none || !valid_strength || !background || (contrast != "on" && contrast != "off")) {
    std::fputs("Usage: terminal-color-vision-demo [protan|deutan|tritan] [0..100] ['#RRGGBB'] [on|off]\n"
        "       terminal-color-vision-demo --report\n", stderr);
    return 1;
  }
  std::printf("%s strength=%d Oklch=%.*s; disable terminal filters when viewing this demo.\n",
      argv[1] ? argv[1] : "deutan", strength, static_cast<int>(contrast.size()), contrast.data());
  std::puts("Original             Recolored            Simulated original   Simulated recolored");
  for (const auto &sample : samples) {
    const auto display = [&](RgbColor color) { return contrast == "on" ? improve_color_visibility(color, *background) : color; };
    const auto before = display(sample.color);
    const auto after = display(correct_color_vision(sample.color, profile, strength / 100.0));
    swatch(before, *background, sample.name);
    swatch(after, *background, sample.name);
    swatch(simulate_color_vision(before, profile), simulate_color_vision(*background, profile), sample.name);
    swatch(simulate_color_vision(after, profile), simulate_color_vision(*background, profile), sample.name);
    std::puts("");
  }
  std::puts("Explicit background samples (white foreground, actual recolored cell background):");
  for (const auto &sample : samples) {
    const auto corrected_background = correct_color_vision(sample.color, profile, strength / 100.0);
    const RgbColor white{255, 255, 255};
    swatch(contrast == "on" ? improve_color_visibility(white, sample.color) : white, sample.color, sample.name);
    swatch(contrast == "on" ? improve_color_visibility(white, corrected_background) : white, corrected_background, sample.name);
    std::puts("");
  }
}
