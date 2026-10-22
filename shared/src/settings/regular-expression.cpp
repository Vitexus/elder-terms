// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/settings/regular-expression.h>

#include <charconv>

namespace elder_terms {

static std::optional<std::size_t> decimal_capture_number(const std::string &capture) {
  if (capture.empty()) return std::nullopt;
  std::size_t group = 0;
  const auto parsed = std::from_chars(capture.data(), capture.data() + capture.size(), group);
  if (parsed.ec != std::errc{} || parsed.ptr != capture.data() + capture.size()) return std::nullopt;
  return group;
}

static const char *compile_error(boxrig_text_regex_diagnostic_code code) {
  switch (code) {
  case BOXRIG_TEXT_REGEX_INVALID_UTF8: return "invalid UTF-8";
  case BOXRIG_TEXT_REGEX_INVALID_ESCAPE: return "invalid escape or Unicode category";
  case BOXRIG_TEXT_REGEX_UNBALANCED_DELIMITER: return "unbalanced delimiter";
  case BOXRIG_TEXT_REGEX_INVALID_RANGE: return "invalid character range";
  case BOXRIG_TEXT_REGEX_UNSUPPORTED_SYNTAX: return "unsupported syntax";
  case BOXRIG_TEXT_REGEX_INVALID_QUANTIFIER: return "invalid quantifier";
  case BOXRIG_TEXT_REGEX_NULLABLE: return "expression must not match empty text";
  case BOXRIG_TEXT_REGEX_PATTERN_LIMIT: return "pattern capacity exceeded";
  case BOXRIG_TEXT_REGEX_QUANTIFIER_LIMIT: return "quantifier capacity exceeded";
  case BOXRIG_TEXT_REGEX_STATE_LIMIT: return "compiled state capacity exceeded";
  case BOXRIG_TEXT_REGEX_CAPTURE_LIMIT: return "capture capacity exceeded";
  case BOXRIG_TEXT_REGEX_DIAGNOSTIC_NONE: return "regular expression compilation failed";
  }
  return "unknown regular expression error";
}

RegularExpressionState *create_regular_expression(const std::string &pattern, std::string *reason) {
  if (reason) reason->clear();
  const auto limits = boxrig_regex_limits_default();
  boxrig_text_regex_diagnostic diagnostic{};
  boxrig_regex_program *program = nullptr;
  const auto status = boxrig_regex_compile(reinterpret_cast<const uint8_t *>(pattern.data()),
      pattern.size(), &limits, &diagnostic, &program);
  if (status != BOXRIG_STATUS_OK && reason) {
    *reason = "at byte " + std::to_string(diagnostic.pattern_offset) + ": " + compile_error(diagnostic.code) +
        " (code " + std::to_string(diagnostic.code) + ", status " + std::to_string(status) + ")";
  }
  return program;
}

bool regular_expression_capture_exists(const RegularExpressionState *state, const std::string &capture) {
  if (!state || capture.empty() || capture.find('\0') != std::string::npos) return false;
  if (capture.front() >= '0' && capture.front() <= '9') {
    const auto number = decimal_capture_number(capture);
    return number && *number <= boxrig_regex_program_group_count(state);
  }
  bool found = false;
  std::size_t group = 0;
  return boxrig_regex_program_find_named_group(state, capture.c_str(), &found, &group) == BOXRIG_STATUS_OK && found;
}

std::optional<RegularExpressionMatch> search_regular_expression(
    RegularExpressionState *state, const std::string &subject, std::string *reason) {
  if (reason) reason->clear();
  boxrig_regex_matcher *matcher = nullptr;
  std::size_t error_offset = 0;
  auto status = boxrig_regex_matcher_create(state, reinterpret_cast<const uint8_t *>(subject.data()),
      subject.size(), 0, &error_offset, &matcher);
  if (status != BOXRIG_STATUS_OK) {
    if (reason) *reason = "input error at byte " + std::to_string(error_offset) +
        " (status " + std::to_string(status) + ")";
    return std::nullopt;
  }
  const std::unique_ptr<boxrig_regex_matcher, decltype(&boxrig_regex_matcher_destroy)> owned(
      matcher, boxrig_regex_matcher_destroy);
  std::size_t work = 0;
  boxrig_regex_progress progress{};
  boxrig_regex_match range{};
  status = boxrig_regex_matcher_resume(matcher, 1000000, &work, &progress, &range);
  if (status != BOXRIG_STATUS_OK || progress == BOXRIG_REGEX_IN_PROGRESS) {
    if (reason) *reason = "matching work limit exceeded or matcher failed (status " + std::to_string(status) + ")";
    return std::nullopt;
  }
  if (progress == BOXRIG_REGEX_NOT_FOUND) return std::nullopt;
  boxrig_regex_result *result = nullptr;
  status = boxrig_regex_matcher_result(matcher, &result);
  if (status != BOXRIG_STATUS_OK) {
    if (reason) *reason = "could not retain captures (status " + std::to_string(status) + ")";
    return std::nullopt;
  }
  return RegularExpressionMatch{range.start, range.end, {result, boxrig_regex_result_destroy}};
}

std::optional<std::string> regular_expression_capture(const boxrig_regex_result *result, const std::string &name) {
  if (!result || name.empty() || name.find('\0') != std::string::npos) return std::nullopt;
  boxrig_regex_capture capture{};
  boxrig_status status;
  if (name.front() >= '0' && name.front() <= '9') {
    const auto group = decimal_capture_number(name);
    if (!group) return std::nullopt;
    status = boxrig_regex_result_get_capture(result, *group, &capture);
  } else {
    status = boxrig_regex_result_get_named_capture(result, name.c_str(), &capture);
  }
  if (status != BOXRIG_STATUS_OK) return std::nullopt;
  if (!capture.participated) return std::string{};
  std::size_t length = 0;
  const auto *bytes = boxrig_regex_result_data(result, &length);
  if (!bytes || capture.start > capture.end || capture.end > length) return std::nullopt;
  return std::string(reinterpret_cast<const char *>(bytes + capture.start), capture.end - capture.start);
}

std::optional<std::string> regular_expression_capture(const RegularExpressionMatch &match, const std::string &capture) {
  return regular_expression_capture(match.result.get(), capture);
}

void destroy_regular_expression(RegularExpressionState *state) { boxrig_regex_program_destroy(state); }

} // namespace elder_terms
