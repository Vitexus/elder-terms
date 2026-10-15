#include "terminal-hyperlink-action.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <elder-terms/settings/regex-capture-template.h>

namespace elder_terms {

static std::optional<std::string> expand_hyperlink_template(
    const std::string &value, const RegexCaptureLookup &capture,
    std::string *reason) {
  return expand_regex_capture_template(
      value, capture,
      RegexCaptureTemplateOptions{.allow_uri_decode = true}, reason);
}

static bool validate_hyperlink_path(const std::string &value,
                                    std::string *reason) {
  if (value.find('\0') != std::string::npos) {
    *reason = "expanded path contains a NUL byte";
    return false;
  }

  const std::filesystem::path path(value);
  if (!path.is_absolute()) {
    *reason = "expanded path must be absolute: " + value;
    return false;
  }

  std::error_code status_error;
  const std::filesystem::file_status status =
      std::filesystem::status(path, status_error);
  if (status_error) {
    if (status_error == std::errc::no_such_file_or_directory) {
      *reason = "expanded path does not exist: " + value;
    } else {
      *reason = "expanded path could not be inspected: " + value + ": " +
                status_error.message();
    }
    return false;
  }
  if (!std::filesystem::exists(status)) {
    *reason = "expanded path does not exist: " + value;
    return false;
  }
  if (!std::filesystem::is_regular_file(status) &&
      !std::filesystem::is_directory(status)) {
    *reason = "expanded path is not a regular file or directory: " + value;
    return false;
  }
  return true;
}

std::optional<TerminalHyperlinkAction> expand_terminal_hyperlink_action(
    const HyperlinkActionRule &rule, const RegexCaptureLookup &capture,
    std::string *reason) {
  TerminalHyperlinkAction action{
      .command = rule.command,
      .arguments = {},
  };
  if (rule.command.empty() || rule.command.find('\0') != std::string::npos) {
    *reason = "command must be nonempty and must not contain a NUL byte";
    return std::nullopt;
  }
  action.arguments.reserve(rule.arguments.size());
  for (const std::string &argument : rule.arguments) {
    const std::optional<std::string> expanded =
        expand_hyperlink_template(argument, capture, reason);
    if (!expanded.has_value()) {
      return std::nullopt;
    }
    if (expanded->find('\0') != std::string::npos) {
      *reason = "expanded argument contains a NUL byte";
      return std::nullopt;
    }
    action.arguments.push_back(*expanded);
  }

  if (rule.path_validation ==
      HyperlinkPathValidation::existing_local_path) {
    const std::optional<std::string> path =
        expand_hyperlink_template(rule.path_template, capture, reason);
    if (!path.has_value() || !validate_hyperlink_path(*path, reason)) {
      return std::nullopt;
    }
  }
  return action;
}

} // namespace elder_terms
