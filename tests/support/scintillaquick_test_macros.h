// Copyright (c) 2026, Ioannis Makris
// Licensed under the BSD 2-Clause License, see LICENSE file for details.

#pragma once

#include <cstdio>

// Shared minimal expectation macro used by the test executables. The
// containing translation unit is expected to define an `int g_failures`
// at namespace scope (typically inside an anonymous namespace) and to
// return its value from main().
#define SQ_EXPECT(expr)                                                           \
    do {                                                                          \
        if (!(expr)) {                                                            \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);  \
            ++g_failures;                                                         \
        }                                                                         \
    }                                                                             \
    while (0)
