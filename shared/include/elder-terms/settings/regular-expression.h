#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <boxrig/regex.h>

#include <elder-terms/export.h>

namespace elder_terms {

/**
 * Immutable boxrig program, also usable by screen recognizers and bounded matchers.
 */
using RegularExpressionState = boxrig_regex_program;

/**
 * Retained boxrig result and its absolute input offsets.
 */
struct RegularExpressionMatch {
  /** UTF-8 byte offset at which the complete match starts. */
  std::size_t start = 0;
  /** UTF-8 byte offset immediately after the complete match. */
  std::size_t end = 0;
  /** Immutable matched bytes and captures; reference operations are serialized by the caller. */
  std::shared_ptr<boxrig_regex_result> result;
};

/**
 * Compiles a UTF-8 boxrig regular expression with finite default capacities.
 *
 * @param pattern Pattern to compile.
 * @param reason Receives a diagnostic and pattern byte offset, or nullptr.
 * @returns Opaque compiled expression, or nullptr when invalid.
 * @remarks Empty matches are rejected. Matching is case-sensitive; anchors
 * refer to the complete input, and dot includes LF. Programs are immutable,
 * but shared use and reference operations must be externally serialized.
 */
ELDER_TERMS_API RegularExpressionState *create_regular_expression(
    const std::string &pattern, std::string *reason);

/**
 * Reports whether a numbered or named capture exists in an expression.
 *
 * @param state Compiled expression.
 * @param capture Decimal capture number or capture name.
 * @returns True when the capture is defined by the expression.
 */
ELDER_TERMS_API bool regular_expression_capture_exists(
    const RegularExpressionState *state, const std::string &capture);

/**
 * Searches a bounded UTF-8 string for the leftmost-longest match.
 *
 * @param state Compiled expression.
 * @param subject UTF-8 subject to search.
 * @param reason Receives a human-readable reason for matching errors.
 * @returns Project-owned match data, or no value when no match is found or an
 * error occurs.
 * @remarks This synchronous helper limits total matching work to 1,000,000
 * operations. Interactive streams use boxrig matchers and resume them between
 * UI dispatches instead. Input capacity is 1 MiB; NUL is accepted.
 */
ELDER_TERMS_API std::optional<RegularExpressionMatch>
search_regular_expression(RegularExpressionState *state,
                          const std::string &subject, std::string *reason);

/**
 * Fetches a numbered or named capture from a match.
 *
 * @param match Match returned by search_regular_expression().
 * @param capture Decimal capture number or capture name.
 * @returns Captured text, or no value when the capture is unknown.
 */
ELDER_TERMS_API std::optional<std::string>
regular_expression_capture(const RegularExpressionMatch &match,
                           const std::string &capture);

/**
 * Reads one capture directly from a retained boxrig result without a name table.
 * @param result Borrowed immutable result, or nullptr.
 * @param capture Decimal number or case-sensitive name; NUL is rejected.
 * @returns Captured text, empty text for an absent group, or no value for an unknown reference.
 */
ELDER_TERMS_API std::optional<std::string> regular_expression_capture(
    const boxrig_regex_result *result, const std::string &capture);

/**
 * Releases a compiled regular expression.
 *
 * @param state Expression returned by create_regular_expression().
 */
ELDER_TERMS_API void
destroy_regular_expression(RegularExpressionState *state);

} // namespace elder_terms
