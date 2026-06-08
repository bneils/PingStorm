#ifndef DEBUG_H
#define DEBUG_H

#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#if DEBUG
#define debug(fmt, ...) printf ("(tid %lu) " fmt "\n", pthread_self () __VA_OPT__(,) __VA_ARGS__)
#define debugstr(buf) printf ("(tid %lu) %s\n", pthread_self (), buf)
#else
#define debug(fmt, ...)
#define debugstr(buf)
#endif

#define WARN (expr) if (expr) PERROR (msg);
#define ASSERT(expr) if (!(expr)) PANIC ("ASSERT(" #expr ")")
#define CHECK(expr) if (expr) PANIC ("CHECK(" #expr ")")
#define PANIC(msg) { \
  PERROR (msg); \
  exit (EXIT_FAILURE); \
}

#define PERROR(msg) \
  fprintf (stderr, "(tid %lu) %s:%d %s: %s (%d)\n", \
    pthread_self (), __FILE__, __LINE__, (msg), \
    (errno) ? strerror (errno) : "panic", errno)

#endif
