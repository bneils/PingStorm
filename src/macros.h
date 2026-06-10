#ifndef MACROS_H
#define MACROS_H

#include <stdlib.h>
#include "logging.h"

#define ASSERT(expr) if (!(expr)) PANIC ("ASSERT(" #expr ")")
#define CHECK(expr) if (expr) PANIC ("CHECK(" #expr ")")

#define PANIC(msg) { \
  log_source (LEVEL_FATAL, msg); \
  exit (EXIT_FAILURE); \
}

#define CLEN(arr) (sizeof (arr) / sizeof (*arr))

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#endif
