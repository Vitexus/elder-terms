#pragma once

#include <boxrig/ui.h>
#include "terminal-hyperlink-action.h"

namespace elder_terms {

/** Owns compiled rules and the current screen recognizers on the UI thread. */
struct TerminalLinks;

/**
 * Creates ordered link recognition against an existing terminal view.
 * @param view Borrowed view, detached before its destruction.
 * @param rules Ordered OSC 8 and visible-text rules.
 * @return Owned link state; invalid rules are diagnosed and omitted.
 */
TerminalLinks *create_terminal_links(boxrig_ui_view *view, std::vector<HyperlinkActionRule> rules);

/**
 * Moves link presentation to another view of the same terminal.
 * @param links Live link state.
 * @param view Borrowed replacement, or nullptr while detached.
 */
void terminal_links_attach_view(TerminalLinks *links, boxrig_ui_view *view);

/**
 * Advances bounded recognition and updates link annotations.
 * @param links Live link state.
 * @param budget Positive maximum recognition work for this dispatch.
 * @return True when another immediate dispatch is needed.
 */
bool terminal_links_dispatch(TerminalLinks *links, std::size_t budget);

/**
 * Resolves a current pointer hit using retained captures and rule priority.
 * @param links Live link state with an attached view.
 * @param hit Revisioned pointer hit; stale and incomplete results are ignored.
 * @return Expanded action, rejection reason, or no matching rule.
 */
std::optional<TerminalHyperlinkResolution> terminal_links_resolve(
    TerminalLinks *links, const boxrig_ui_hit_test_result &hit);

/**
 * Clears annotations and unregisters recognizers before releasing rules.
 * @param links Owned state, or nullptr.
 */
void destroy_terminal_links(TerminalLinks *links);

}
