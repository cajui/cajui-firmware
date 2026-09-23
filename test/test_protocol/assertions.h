#pragma once
#include <unity.h>

// Macros, not functions: a helper function would report its own line on failure.
#define EXPECT_RESULT(expected, actual) TEST_ASSERT_EQUAL_INT(int(expected), int(actual))
