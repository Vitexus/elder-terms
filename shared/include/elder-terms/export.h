// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

/**
 * Marks a public elder-terms shared-library symbol.
 */
#if defined(__GNUC__) || defined(__clang__)
#define ELDER_TERMS_API __attribute__((visibility("default")))
#else
#define ELDER_TERMS_API
#endif
