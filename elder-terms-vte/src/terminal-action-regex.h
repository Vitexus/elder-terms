#pragma once

#include <boxrig/regex.h>
#include <memory>
#include <optional>
#include <string>

namespace elder_terms {

/** Shared immutable program; the UI thread serializes reference operations. */
using TerminalActionRegex = std::shared_ptr<boxrig_regex_program>;
/** Immutable retained match; captures are queried from boxrig without a name table. */
using TerminalActionRegexResult = std::shared_ptr<boxrig_regex_result>;

/**
 * Compiles bounded boxrig syntax without creating a terminal or matcher.
 * @param pattern UTF-8 pattern, including any embedded NUL bytes.
 * @param reason Receives a diagnostic with its pattern byte offset.
 * @return Shared program, or an empty pointer on failure.
 */
TerminalActionRegex compile_terminal_action_regex(const std::string &pattern, std::string *reason);

/**
 * Looks up one capture in an immutable result.
 * @param result Borrowed result retained by the caller.
 * @param capture Number or case-sensitive name; embedded NUL is rejected.
 * @return Text, empty text for an absent group, or no value for an unknown reference.
 */
std::optional<std::string> terminal_action_regex_capture(
    const boxrig_regex_result *result, const std::string &capture);

}
