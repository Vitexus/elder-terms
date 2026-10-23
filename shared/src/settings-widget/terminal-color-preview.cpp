// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "terminal-color-preview.h"
#include <elder-terms/terminal-color-filter.h>
#include <array>
#include <cstdio>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {
struct TerminalColorPreview {
  SettingsStore *store;
  GtkWidget *root;
  GtkWidget *background;
  std::array<GtkWidget *, 2> labels;
  std::array<GtkCssProvider *, 2> styles;
  std::shared_ptr<const ReversePalette> palette{};
};

static void identify(GtkWidget *widget, const std::string &id) {
  gtk_widget_set_name(widget, id.c_str());
  atk_object_set_accessible_id(gtk_widget_get_accessible(widget), id.c_str());
}

static std::string span(const char *text, RgbColor foreground, RgbColor background,
    bool explicit_background, TerminalColorFilterContext &context) {
  boxrig_ui_color_filter_input input{};
  input.target = BOXRIG_UI_COLOR_CELL;
  input.colors.foreground = {foreground.red, foreground.green, foreground.blue};
  input.colors.background = {background.red, background.green, background.blue};
  input.foreground_source.role = BOXRIG_UI_COLOR_RGB;
  input.background_source.role = explicit_background ? BOXRIG_UI_COLOR_RGB : BOXRIG_UI_COLOR_DEFAULT_BACKGROUND;
  const auto colors = filter_terminal_colors(&input, &context);
  auto *markup = g_markup_printf_escaped(
      "<span foreground=\"#%02x%02x%02x\" background=\"#%02x%02x%02x\">%s</span>",
      colors.foreground.red, colors.foreground.green, colors.foreground.blue,
      colors.background.red, colors.background.green, colors.background.blue, text);
  const std::string result(markup);
  g_free(markup);
  return result;
}

static std::string samples(TerminalColorFilterContext context) {
  const auto background = *context.background;
  const auto neutral = improve_color_visibility({255, 255, 255}, background);
  std::string result = "<span font_family=\"monospace\">";
  result += span("ANSI 16                         ", neutral, background, false, context) + "\n";
  constexpr std::array<RgbColor, 16> palette{{{0, 0, 0}, {128, 0, 0}, {0, 128, 0}, {128, 128, 0},
      {0, 0, 128}, {128, 0, 128}, {0, 128, 128}, {192, 192, 192}, {128, 128, 128},
      {255, 0, 0}, {0, 255, 0}, {255, 255, 0}, {0, 0, 255}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255}}};
  for (std::size_t i = 0; i < palette.size(); i++) {
    char text[32];
    std::snprintf(text, sizeof(text), " %02zu ", i);
    // Numbers identify the swatches independently of their colors.
    const auto ink = improve_color_visibility({255, 255, 255}, palette[i]);
    result += span(text, ink, palette[i], true, context);
    if (i == 7 || i == 15) result += "\n";
  }
  result += span("- removed       ", {128, 0, 0}, background, false, context);
  result += span("+ added         ", {0, 128, 0}, background, false, context) + "\n";
  result += span("WARN 256 #166   ", {215, 95, 0}, background, false, context);
  result += span("INFO 256 #37    ", {0, 175, 175}, background, false, context) + "\n";
  result += span("ERROR RGB       ", {200, 80, 80}, background, false, context);
  result += span("OK RGB          ", {80, 128, 80}, background, false, context) + "\n";
  result += span(" ERROR BG       ", {255, 255, 255}, {200, 80, 80}, true, context);
  result += span(" OK BG          ", {255, 255, 255}, {80, 128, 80}, true, context);
  return result + "</span>";
}

void sync_terminal_color_preview(TerminalColorPreview *preview) {
  if (!preview) return;
  const int selected = gtk_combo_box_get_active(GTK_COMBO_BOX(preview->background));
  const auto background = selected == 1 ? RgbColor{255, 255, 255} : selected == 2
      ? general_color_settings(*preview->store).background.value_or(RgbColor{0, 0, 0}) : RgbColor{32, 32, 32};
  const auto vision = terminal_color_vision_settings(*preview->store);
  TerminalColorFilterContext before{background, TerminalColorFilter::none};
  TerminalColorFilterContext after{background, terminal_color_filter(*preview->store),
      vision.profile, vision.strength / 100.0, preview->palette};
  if (!prepare_terminal_color_filter(&after, nullptr)) g_warning("Failed to prepare preview palette");
  preview->palette = after.reverse_palette;
  gtk_label_set_markup(GTK_LABEL(preview->labels[0]), samples(before).c_str());
  gtk_label_set_markup(GTK_LABEL(preview->labels[1]), samples(after).c_str());
  for (int index = 0; index < 2; index++) {
    boxrig_ui_color_filter_input input{};
    input.target = BOXRIG_UI_COLOR_SURFACE;
    input.background_source.role = BOXRIG_UI_COLOR_DEFAULT_BACKGROUND;
    const auto surface = filter_terminal_colors(&input, index == 0 ? &before : &after).background;
    char css[128];
    std::snprintf(css, sizeof(css), "label { background-color: #%02x%02x%02x; padding: 6px; }",
        surface.red, surface.green, surface.blue);
    gtk_css_provider_load_from_data(preview->styles[index], css, -1, nullptr);
  }
}

TerminalColorPreview *create_terminal_color_preview(SettingsStore *store, const std::string &prefix) {
  auto *preview = new TerminalColorPreview{store, gtk_expander_new(_("Compare colors")), gtk_combo_box_text_new(), {},
      {gtk_css_provider_new(), gtk_css_provider_new()}};
  g_object_ref_sink(preview->root);
  identify(preview->root, prefix + "_terminal_color_preview");
  auto *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  auto *description = gtk_label_new(_("Compare the samples and choose colors you can distinguish. Changes are applied with Apply or Save."));
  gtk_label_set_line_wrap(GTK_LABEL(description), TRUE);
  gtk_label_set_xalign(GTK_LABEL(description), 0);
  gtk_label_set_max_width_chars(GTK_LABEL(description), 60);
  gtk_box_pack_start(GTK_BOX(body), description, FALSE, FALSE, 0);
  for (const auto *label : {_("Dark background"), _("Light background"), _("Connection background")})
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(preview->background), label);
  gtk_combo_box_set_active(GTK_COMBO_BOX(preview->background), 0);
  identify(preview->background, prefix + "_terminal_color_preview_background");
  gtk_box_pack_start(GTK_BOX(body), preview->background, FALSE, FALSE, 0);
  auto *comparison = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(comparison), 8);
  gtk_grid_set_column_homogeneous(GTK_GRID(comparison), TRUE);
  for (int index = 0; index < 2; index++) {
    auto *heading = gtk_label_new(index == 0 ? _("Before correction") : _("With current settings"));
    gtk_grid_attach(GTK_GRID(comparison), heading, index, 0, 1, 1);
    auto *label = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    identify(label, prefix + (index == 0 ? "_terminal_color_preview_before" : "_terminal_color_preview_after"));
    gtk_style_context_add_provider(gtk_widget_get_style_context(label), GTK_STYLE_PROVIDER(preview->styles[index]), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    preview->labels[index] = label;
    gtk_grid_attach(GTK_GRID(comparison), label, index, 1, 1, 1);
  }
  gtk_box_pack_start(GTK_BOX(body), comparison, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(preview->root), body);
  g_signal_connect(preview->background, "changed", G_CALLBACK(+[](GtkComboBox *, gpointer data) {
    sync_terminal_color_preview(static_cast<TerminalColorPreview *>(data));
  }), preview);
  sync_terminal_color_preview(preview);
  return preview;
}

GtkWidget *terminal_color_preview_root(TerminalColorPreview *preview) { return preview->root; }

void destroy_terminal_color_preview(TerminalColorPreview *preview) {
  if (!preview) return;
  gtk_widget_destroy(preview->root);
  g_object_unref(preview->root);
  for (auto *style : preview->styles) g_object_unref(style);
  delete preview;
}
}
