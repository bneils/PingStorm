#ifndef MACROS_H
#define MACROS_H

#include <stdlib.h>
#include "logging.h"

#define ASSERT(expr, ...) if (!(expr)) { __VA_ARGS__; PANIC ("ASSERT(" #expr ")") }
#define CHECK(expr, ...) if (expr) { __VA_ARGS__; PANIC ("CHECK(" #expr ")") }

// An assert that returns the value of the expression
#define ASSERT_CMP(expr, cmp_op, ...) ({ \
  typeof (expr) __av = (expr); \
  if (!(__av cmp_op)) { \
    __VA_ARGS__; \
    PANIC ("ASSERT(" #expr " " #cmp_op ")"); \
  } \
  __av; })

#define ASSERT_EQ(expr, test, ...) ASSERT_CMP(expr, == test __VA_OPT__(,) __VA_ARGS__)
#define ASSERT_NOT(expr, test, ...) ASSERT_CMP(expr, != test __VA_OPT__(,) __VA_ARGS__)
#define ASSERT_LT(expr, test, ...) ASSERT_CMP(expr, < test __VA_OPT__(,) __VA_ARGS__)
#define ASSERT_LE(expr, test, ...) ASSERT_CMP(expr, <= test __VA_OPT__(,) __VA_ARGS__)
#define ASSERT_GT(expr, test, ...) ASSERT_CMP(expr, > test __VA_OPT__(,) __VA_ARGS__)
#define ASSERT_GE(expr, test, ...) ASSERT_CMP(expr, >= test __VA_OPT__(,) __VA_ARGS__)

#define PANIC(msg) { \
  log_source (LEVEL_FATAL, msg); \
  exit (EXIT_FAILURE); \
}

#define CLEN(arr) (sizeof (arr) / sizeof (*arr))

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#endif
