// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <elder-terms/export.h>

namespace elder_terms {

/**
 * Returns the version of the running elder-terms build.
 *
 * @returns NUL-terminated version string embedded at build time.
 */
ELDER_TERMS_API const char *application_version();

} // namespace elder_terms
