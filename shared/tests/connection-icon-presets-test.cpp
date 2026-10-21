#include <elder-terms/connection-icon-presets.h>

#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  try {
    const auto &catalog = elder_terms::connection_icon_presets();
    std::unordered_set<std::string> specifications;
    std::size_t noto_count = 0;
    std::size_t material_count = 0;
    for (const auto &preset : catalog) {
      expect(!preset.name.empty() && !preset.specification.empty(),
             "every preset needs a stable ID and English name");
      expect(specifications.insert(preset.specification).second,
             "preset specifications must be unique");
      if (preset.family == elder_terms::ConnectionIconFamily::Noto) {
        ++noto_count;
        expect(preset.specification.starts_with("noto:"),
               "Noto IDs must use their source artwork identifier");
      } else {
        ++material_count;
        expect(preset.specification.starts_with("material:"),
               "Material IDs must use their source category and icon name");
      }
    }
    expect(noto_count > 0 && material_count > 0,
           "both preset families must offer selectable icons");
    for (const char *specification : {
             "noto:emoji_u1f44d", "material:action/home"}) {
      const auto image = elder_terms::connection_icon_preset_thumbnail(
          specification, 64);
      expect(image != nullptr && gdk_pixbuf_get_width(image.get()) == 64 &&
                 gdk_pixbuf_get_height(image.get()) == 64 &&
                 gdk_pixbuf_get_has_alpha(image.get()),
             "both preset families must resolve to square RGBA thumbnails");
    }
    expect(elder_terms::connection_icon_preset_thumbnail(
               "material:action/unknown", 24) == nullptr &&
               elder_terms::connection_icon_preset_thumbnail(
                   "noto:emoji_u1f44d", 0) == nullptr,
           "unknown IDs and invalid sizes must not yield an image");
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
