// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <functional>
#include <string>

namespace elder_terms {

/**
 * Describes presentation callbacks for one terminal BREAK request.
 */
struct TerminalBreakRequest {
  /** Receives the final user-facing BREAK status. */
  std::function<void(std::string status)> status;
  /** Receives completion after backend cleanup finishes. */
  std::function<void(bool succeeded)> finished;
};

} // namespace elder_terms
