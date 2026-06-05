#ifndef DEBUG_H
#define DEBUG_H

#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#if DEBUG
#define debug(fmt, ...) printf ("(tid %lu) " fmt "\n", pthread_self () __VA_OPT__(,) __VA_ARGS__)
#else
#define debug(fmt, ...)
#endif

#define ASSERT(expr) CHECK(!(expr))
#define CHECK(expr) if (expr) PANIC (#expr)
#define PANIC(msg) { \
  fprintf (stderr, "(tid %lu) %s:%d (%s): %s\n", pthread_self (), __FILE__, __LINE__, (msg), (errno) ? strerror (errno) : "panic"); \
  exit (EXIT_FAILURE); \
}

#endif
