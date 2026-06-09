#ifndef LOGGING_H

#include <stdio.h>
#include <errno.h>
#include <string.h>

// Reference https://en.wikipedia.org/wiki/Log4j#Log4j_log_levels

enum LogLevel {
  LEVEL_TRACE,
  LEVEL_DEBUG,
  LEVEL_INFO,
  LEVEL_WARN,
  LEVEL_ERROR,
  LEVEL_FATAL,
  LEVEL_OFF
};

#ifndef LOG_LEVEL
#define LOG_LEVEL LEVEL_INFO
#endif

extern const char *log_level_strs[];

// Emit expression only if dereferenced pointer is non-zero
#define EMIT_IF_LOGLEVEL(level, rettype, expr) if ((level) >= LOG_LEVEL) expr; else /* ; */

#define LOG_FD(level) \
  ((level >= LEVEL_WARN) ? stderr : stdout)

#define log(level, fmt, ...) \
  EMIT_IF_LOGLEVEL (level, int, fprintf (LOG_FD (level), "%s " fmt "\n", log_level_strs[level] __VA_OPT__(,) __VA_ARGS__))
#define log_tid(level, fmt, ...) \
  EMIT_IF_LOGLEVEL (level, int, fprintf (LOG_FD (level), "%s (tid %lu) " fmt "\n", log_level_strs[level], pthread_self () __VA_OPT__(,) __VA_ARGS__))
#define log_buf(level, fmtbuf, ...) \
  EMIT_IF_LOGLEVEL (level, int, fprintf (LOG_FD (level), fmtbuf, log_level_strs[level] __VA_OPT__(,) __VA_ARGS__))
#define log_source(level, msg) \
  log (level, "%s:%d %s: %s (%d)", __FILE__, __LINE__, (msg), (errno) ? strerror (errno) : "panic", errno)

#endif
