#include "terminal-action-regex.h"

#include <charconv>

namespace elder_terms {

TerminalActionRegex compile_terminal_action_regex(const std::string &pattern, std::string *reason) {
  const auto limits = boxrig_regex_limits_default();
  boxrig_text_regex_diagnostic diagnostic{};
  boxrig_regex_program *program = nullptr;
  const auto status = boxrig_regex_compile(reinterpret_cast<const uint8_t *>(pattern.data()),
      pattern.size(), &limits, &diagnostic, &program);
  if (status != BOXRIG_STATUS_OK) {
    *reason = "pattern error at byte " + std::to_string(diagnostic.pattern_offset) +
        " (code " + std::to_string(diagnostic.code) + ", status " + std::to_string(status) + ")";
    return {};
  }
  return {program, boxrig_regex_program_destroy};
}

std::optional<std::string> terminal_action_regex_capture(
    const boxrig_regex_result *result, const std::string &name) {
  if (!result || name.empty() || name.find('\0') != std::string::npos) return std::nullopt;
  boxrig_regex_capture capture{};
  boxrig_status status;
  if (name.front() >= '0' && name.front() <= '9') {
    std::size_t group = 0;
    const auto parsed = std::from_chars(name.data(), name.data() + name.size(), group);
    if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size()) return std::nullopt;
    status = boxrig_regex_result_get_capture(result, group, &capture);
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

}
