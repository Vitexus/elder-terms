#pragma once
#include <boxrig/gtk3/terminal_widget.h>
#include <string>

namespace elder_terms {

/** Read-only presentation information; never stored as a preference. */
struct TerminalRendererStatus {
  /** Short footer text describing the last completed backend. */
  std::string text;
  /** Complete text with pending or fallback diagnostics. */
  std::string tooltip;
};

/**
 * Formats a completed rendering state without guessing from requested policy.
 * @param state Latest native state, including initial and pending states.
 * @param display Actual window system name supplied by the display connection.
 * @return Localized text and diagnostics.
 */
TerminalRendererStatus format_terminal_renderer_status(const boxrig_gtk_render_state &state,
    const std::string &display);

/**
 * Reads the widget's current backend and its actual GDK display connection.
 * @param widget Live terminal widget, owned by the runtime.
 * @return Initializing before the first successful presentation.
 */
TerminalRendererStatus terminal_renderer_status(BoxrigGtk3TerminalWidget *widget);

}
