// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/application-version.h>

#include "application-version-config.h"

namespace elder_terms {

const char *application_version() {
  return ELDER_TERMS_APPLICATION_VERSION;
}

} // namespace elder_terms
