#ifndef LOGGING_H
#define LOGGING_H

#include <stdio.h>
#include <errno.h>
#include <string.h>

// Reference https://en.wikipedia.org/wiki/Log4j#Log4j_log_levels

enum LogLevel {
  LEVEL_TRACE  = 0,
  LEVEL_DEBUG  = 1,
  LEVEL_INFO   = 2,
  LEVEL_WARN   = 3,
  LEVEL_ERROR  = 4,
  LEVEL_FATAL  = 5,
  LEVEL_OFF    = 6,
};

#ifndef LOG_LEVEL
#define LOG_LEVEL LEVEL_INFO
#endif

extern const char *log_level_strs[];
extern FILE *log_file;
extern int log_file_level;

void ffprintf (FILE *f1, FILE *f2, char *fmt, ...);

// Emit expression only if dereferenced pointer is non-zero
#define EMIT_IF_LOGLEVEL(level, expr) (void)(((level) >= LOG_LEVEL) ? expr : 0)

#define LOG_FD(level) \
  ((level >= LEVEL_WARN) ? stderr : stdout)

// line 1. the first ternary lets the compiler optimize the check out, since (true || ?) is always true.
// line 2. two ternaries to get the files assoc. with the log level (stdout/stderr) and the log file,
//   which is passed to ffprintf along with the variadic arguments.
// result: when compiled, this should result in: (an unlikely) IF, then function call with: constant-time argument,
// mandatory IF check, and the arguments.
// I think this is not too bloated for a macro since the compiler can optimize it easily :)
#define log(level, fmt, ...) \
  (\
    (\
      (level) >= LOG_LEVEL || (level) >= log_file_level\
    ) ? ( \
      ffprintf (\
        ((level) >= LOG_LEVEL) ? LOG_FD (level) : NULL,\
        ((level) >= log_file_level) ? log_file : NULL,\
        "%s " fmt "\n",\
        log_level_strs[level]\
        __VA_OPT__(,) __VA_ARGS__\
      )\
    ) : (void)0\
  )

#define log_tid(level, fmt, ...) \
  log ((level), "(tid %lu) " fmt, pthread_self (), __VA_OPT__(,) __VA_ARGS__)

/* newline is not provided, %s is mandatory at beginning of fmtbuf */
#define log_buf(level, fmtbuf, ...) \
  (void)(((level) >= LOG_LEVEL || (level) >= log_file_level) ? ( \
    ffprintf ((level) >= LOG_LEVEL ? LOG_FD (level) : NULL, ((level) >= log_file_level) ? log_file : NULL, \
      fmtbuf, log_level_strs[level] __VA_OPT__(,) __VA_ARGS__))) : 0)

#define log_source(level, msg) \
  log (level, "%s:%d %s: %s (%d)", __FILE__, __LINE__, (msg), (errno) ? strerror (errno) : "panic", errno)

#endif
