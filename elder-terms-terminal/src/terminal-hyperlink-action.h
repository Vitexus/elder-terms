#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <elder-terms/settings/hyperlink-settings.h>
#include <elder-terms/settings/regex-capture-template.h>

namespace elder_terms {

/**
 * External command resolved from one terminal link candidate.
 */
struct TerminalHyperlinkAction {
  /** Fixed executable name or path from the matching rule. */
  std::string command;
  /** Expanded ordered arguments, excluding argv[0]. */
  std::vector<std::string> arguments;
};

/**
 * Result of evaluating terminal link candidates against ordered rules.
 */
struct TerminalHyperlinkResolution {
  /** Safe expanded action, or no value when validation rejected the match. */
  std::optional<TerminalHyperlinkAction> action;
  /** Human-readable rejection reason when action has no value. */
  std::string error;
  /** Recognizer identity for a text link, or zero for OSC 8. */
  std::uint64_t source_id = 0;
  /** Match identity within source_id, or zero for OSC 8. */
  std::uint64_t match_id = 0;
  /** Terminal-lifetime OSC 8 identity, or zero for a text link. */
  std::uint32_t hyperlink_handle = 0;
};

/**
 * Expands and validates an action using already matched captures.
 * @param rule Rule whose command and path policy are applied.
 * @param capture Borrowed lookup into an immutable match.
 * @param reason Receives a template or path rejection reason.
 * @return Safe command and arguments, or no value on rejection.
 */
std::optional<TerminalHyperlinkAction> expand_terminal_hyperlink_action(
    const HyperlinkActionRule &rule, const RegexCaptureLookup &capture, std::string *reason);

} // namespace elder_terms
