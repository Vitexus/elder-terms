#include "connection-icon-editor.h"
#include "connection-icon-picker.h"
#include <elder-terms/connection-icon-image.h>
#include <elder-terms/connection-icon-presets.h>
#include <elder-terms/modal-dialog.h>
#include <elder-terms/settings/general-settings.h>
#include <memory>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

struct ConnectionIconEditor {
  SettingsStore *store;
  std::string prefix;
  std::function<void()> changed;
  GtkWidget *root;
  GtkWidget *preview;
  GtkWidget *selection;
  GtkWidget *status;
  GtkWidget *dialog = nullptr;
  GtkWidget *picker = nullptr;
  std::shared_ptr<ConnectionIconEditor *> owner;
};

static void identify(GtkWidget *widget, const std::string &id) {
  gtk_widget_set_name(widget, id.c_str());
  atk_object_set_accessible_id(gtk_widget_get_accessible(widget), id.c_str());
}

void sync_connection_icon_editor(ConnectionIconEditor *editor) {
  if (!editor) return;
  const auto specification = general_connection_icon(*editor->store);
  std::string label = _("Default icon");
  if (specification.starts_with("file:")) {
    auto *name = g_path_get_basename(specification.c_str() + 5);
    label = name;
    g_free(name);
  } else if (specification != "default" && !specification.empty()) {
    label = specification;
    for (const auto &preset : connection_icon_presets()) {
      if (preset.specification == specification) {
        label = preset.name;
        if (preset.family == ConnectionIconFamily::Material)
          label += " (" + std::string(_("Black")) + ")";
        break;
      }
      if (preset.family == ConnectionIconFamily::Material &&
          specification == "material:white:" + preset.specification.substr(
              std::string("material:").size())) {
        label = preset.name + " (" + std::string(_("White")) + ")";
        break;
      }
    }
  }
  gtk_label_set_text(GTK_LABEL(editor->selection), label.c_str());
  gtk_widget_set_tooltip_text(editor->selection, specification.c_str());
  set_connection_icon_image(GTK_IMAGE(editor->preview), specification, 32,
      [owner = editor->owner](const std::string &error) {
        if (auto *editor = *owner) {
          gtk_label_set_text(GTK_LABEL(editor->status), error.c_str());
          gtk_widget_set_visible(editor->status, !error.empty());
        }
      });
}

static void set_selection(ConnectionIconEditor *editor, std::string specification) {
  set_explicit_setting_value(editor->store, general_icon_setting_key(), std::move(specification));
  sync_connection_icon_editor(editor);
  editor->changed();
}

static void choose_file(GtkButton *, gpointer data) {
  auto *editor = static_cast<ConnectionIconEditor *>(data);
  if (editor->dialog) {
    present_modal_dialog(editor->dialog);
    return;
  }
  auto *top = gtk_widget_get_toplevel(editor->root);
  auto *parent = GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : nullptr;
  auto *dialog = gtk_file_chooser_dialog_new(_("Select a connection icon"), parent,
      GTK_FILE_CHOOSER_ACTION_OPEN, _("Cancel"), GTK_RESPONSE_CANCEL,
      _("Open"), GTK_RESPONSE_ACCEPT, nullptr);
  editor->dialog = dialog;
  identify(dialog, editor->prefix + "_general_icon_dialog");
  identify(gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT),
      editor->prefix + "_general_icon_open");
  gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(dialog), TRUE);
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), FALSE);
  auto *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, _("PNG and JPEG images"));
  gtk_file_filter_add_mime_type(filter, "image/png");
  gtk_file_filter_add_mime_type(filter, "image/jpeg");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
  const auto current = general_connection_icon(*editor->store);
  if (current.starts_with("file:")) {
    auto *filename = g_filename_from_utf8(current.c_str() + 5, -1, nullptr, nullptr, nullptr);
    if (filename) gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), filename);
    g_free(filename);
  }
  g_signal_connect(dialog, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
    static_cast<ConnectionIconEditor *>(data)->dialog = nullptr;
  }), editor);
  g_signal_connect(dialog, "response", G_CALLBACK(+[](GtkDialog *dialog, gint response, gpointer data) {
    auto *editor = static_cast<ConnectionIconEditor *>(data);
    if (response == GTK_RESPONSE_ACCEPT) {
      auto *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
      if (filename) {
        auto *text = g_filename_to_utf8(filename, -1, nullptr, nullptr, nullptr);
        if (text) set_selection(editor, std::string("file:") + text);
        g_free(text);
        g_free(filename);
      }
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
  }), editor);
  show_modal_dialog(dialog, parent);
}

static void choose_icon(GtkButton *button, gpointer data) {
  auto *editor = static_cast<ConnectionIconEditor *>(data);
  if (!editor->picker) {
    editor->picker = create_connection_icon_picker(GTK_WIDGET(button), editor->prefix,
        [owner = editor->owner](std::string specification) {
          if (auto *editor = *owner) set_selection(editor, std::move(specification));
        }, [owner = editor->owner] {
          if (auto *editor = *owner) choose_file(nullptr, editor);
        });
    g_signal_connect(editor->picker, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
      static_cast<ConnectionIconEditor *>(data)->picker = nullptr;
    }), editor);
  }
  sync_connection_icon_picker(editor->picker, general_connection_icon(*editor->store));
  gtk_widget_show(editor->picker);
}

ConnectionIconEditor *create_connection_icon_editor(SettingsStore *store,
    std::string prefix, std::function<void()> changed) {
  auto *editor = new ConnectionIconEditor{store, std::move(prefix), std::move(changed),
      gtk_box_new(GTK_ORIENTATION_VERTICAL, 4), gtk_image_new(), gtk_label_new(nullptr),
      gtk_label_new(nullptr), nullptr, nullptr, {}};
  editor->owner = std::make_shared<ConnectionIconEditor *>(editor);
  auto *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  identify(editor->preview, editor->prefix + "_general_icon_preview");
  identify(editor->selection, editor->prefix + "_general_icon_selection");
  identify(editor->status, editor->prefix + "_general_icon_status");
  gtk_widget_set_size_request(editor->preview, 32, 32);
  gtk_box_pack_start(GTK_BOX(row), editor->preview, FALSE, FALSE, 0);
  gtk_label_set_xalign(GTK_LABEL(editor->selection), 0);
  gtk_label_set_ellipsize(GTK_LABEL(editor->selection), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_max_width_chars(GTK_LABEL(editor->selection), 24);
  gtk_box_pack_start(GTK_BOX(row), editor->selection, TRUE, TRUE, 0);
  auto *choose = gtk_button_new_with_label(_("Choose icon…"));
  identify(choose, editor->prefix + "_general_icon_choose");
  g_signal_connect(choose, "clicked", G_CALLBACK(choose_icon), editor);
  gtk_box_pack_start(GTK_BOX(row), choose, FALSE, FALSE, 0);
  auto *reset = gtk_button_new_with_label(_("Default icon"));
  identify(reset, editor->prefix + "_general_icon_reset");
  g_signal_connect(reset, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
    set_selection(static_cast<ConnectionIconEditor *>(data), "default");
  }), editor);
  gtk_box_pack_start(GTK_BOX(row), reset, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(editor->root), row, FALSE, FALSE, 0);
  gtk_label_set_xalign(GTK_LABEL(editor->status), 0);
  gtk_label_set_line_wrap(GTK_LABEL(editor->status), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(editor->status), 48);
  gtk_widget_set_no_show_all(editor->status, TRUE);
  gtk_box_pack_start(GTK_BOX(editor->root), editor->status, FALSE, FALSE, 0);
  sync_connection_icon_editor(editor);
  return editor;
}

GtkWidget *connection_icon_editor_root(ConnectionIconEditor *editor) { return editor->root; }

void destroy_connection_icon_editor(ConnectionIconEditor *editor) {
  if (!editor) return;
  *editor->owner = nullptr;
  if (editor->picker) gtk_widget_destroy(editor->picker);
  if (editor->dialog) gtk_widget_destroy(editor->dialog);
  delete editor;
}

} // namespace elder_terms
