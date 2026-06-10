#include "logging.h"
#include <stdarg.h>
#include <malloc.h>

FILE *log_file;
int log_file_level = LEVEL_OFF;

// https://ansi.gabebanks.net/
/*const char *log_level_strs[] = {
  "\033[34;49m[t]\033[0m",  // trace
  "\033[32;49m[d]\033[0m",  // debug
  "\033[37;49;1m[i]\033[0m",  // info
  "\033[33;49m[w]\033[0m",  // warn
  "\033[31;49m[e]\033[0m",  // error
  "\033[31;49;1m[f]\033[0m",  // fatal
  ""      // off
};*/

const char *log_level_strs[] = {
  "[t]",
  "[d]",
  "[i]",
  "[w]",
  "[e]",
  "[f]",
  ""      // off
};

/* print to files f1 and f2, skipping either if NULL is given, then flushing both */
void
ffprintf (FILE *f1, FILE *f2, char *fmt, ...)
{
  // https://stackoverflow.com/questions/2288680/reuse-of-va-list
  // ^ very deadly bug :(
  va_list args, dup_args;

  // initialize both lists
  va_start (args, fmt);
  va_copy (dup_args, args);

  if (f1) {
    vfprintf(f1, fmt, args);
    fflush (f1);
  }
  if (f2) {
    vfprintf(f2, fmt, dup_args);
    fflush (f2);
  }

  // cleanup
  va_end (args);
  va_end (dup_args);
}
