// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <unity.h>
#include <cstdio>

// Macros, not functions: a helper function would report its own line on failure.
#define EXPECT_RESULT(expected, actual) TEST_ASSERT_EQUAL_INT(int(expected), int(actual))

// Names the current table row in failure output; Unity clears it after each test.
inline const char* scenarioName(long index) {
    static char text[24];
    std::snprintf(text, sizeof(text), "%ld", index);
    return text;
}
#define SCENARIO(index) UNITY_SET_DETAIL(scenarioName(long(index)))
