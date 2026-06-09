#include "logging.h"

// https://ansi.gabebanks.net/
const char *log_level_strs[] = {
  "\033[34;49m[t]\033[0m",  // trace
  "\033[32;49m[d]\033[0m",  // debug
  "\033[37;49;1m[i]\033[0m",  // info
  "\033[33;49m[w]\033[0m",  // warn
  "\033[31;49m[e]\033[0m",  // error
  "\033[31;49;1m[f]\033[0m",  // fatal
  ""      // off
};
