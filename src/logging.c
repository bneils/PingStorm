#include "logging.h"
#include <stdarg.h>
#include <malloc.h>
#include <time.h>
#include <pthread.h>

FILE *log_file;
int log_file_level = LEVEL_OFF;

// https://ansi.gabebanks.net/
const char *log_level_strs_color[] = {
  "\033[34;49m[t]\033[0m",  // trace
  "\033[32;49m[d]\033[0m",  // debug
  "\033[37;49;1m[i]\033[0m",  // info
  "\033[33;49m[w]\033[0m",  // warn
  "\033[31;49m[e]\033[0m",  // error
  "\033[31;49;1m[f]\033[0m",  // fatal
  ""      // off
};

const char *log_level_strs[] = {
  "[t]", // trace
  "[d]", // debug
  "[i]", // info
  "[w]", // warn
  "[e]", // error
  "[f]", // fatal
  ""     // off
};

/* print to console and log, skipping either if NULL is given, then flushing both. Includes date/time in log. */
void
wlog (int level, char *fmt, ...)
{
  // https://stackoverflow.com/questions/2288680/reuse-of-va-list
  // ^ very deadly bug :(
  va_list args, log_args;
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

  // Concurrent access to the log file and stdout is protected by a mutex.
  // This is just to keep the logs from looking weird.
  pthread_mutex_lock (&lock);

  // initialize both lists
  va_start (args, fmt);
  va_copy (log_args, args);

  if (level >= LOG_LEVEL) {
    FILE *f = LOG_FD (level);
    fprintf (f, "%s ", log_level_strs_color[level]);
    vfprintf (f, fmt, args);
    fprintf (f, "\n");
    fflush (f);
  }
  if (level >= log_file_level && log_file) {
    // https://stackoverflow.com/questions/10917491/building-a-date-string-in-c
    char datestr[100];
    time_t now = time (NULL);
    struct tm *t = localtime (&now);
    strftime (datestr, sizeof (datestr) - 1, "%Y-%m-%d %H:%M", t);
    fprintf (log_file, "%s %s ", datestr, log_level_strs[level]);

    vfprintf (log_file, fmt, log_args);
    fprintf (log_file, "\n");
    fflush (log_file);
  }

  pthread_mutex_unlock (&lock);

  // cleanup
  va_end (args);
  va_end (log_args);
}
