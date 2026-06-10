#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>

#include "macros.h"
#include "ping.h"
#include "worker.h"
#include "config.h"

static void config_parse_line_opt (const char *line, size_t len, size_t *left_len, size_t *right, size_t *right_len);
static int parse_long (char *s, unsigned long *n, int base);

static void
config_parse_line_opt (const char *line, size_t len, size_t *left_len, size_t *right, size_t *right_len)
{
  // [a-zA-Z_]+\s*=\s*[a-zA-Z_0-9]+

  if (!isalpha (line[0]) && line[0] != '_') {
    wlog (LEVEL_FATAL, "config_parse_opt: '%s': lvalue must start with an alphabetical", line);
    exit (EXIT_FAILURE);
  }

  // Find the equal sign delimiter and first non matching in left option
  size_t equal_sign = 0, non_alpha = 0;
  for (size_t i = 0; i < len; ++i) {
    if (!equal_sign && line[i] == '=')
      equal_sign = i;

    if (!non_alpha && (!isalpha (line[i]) && line[i] != '_'))
      non_alpha = i;
  }

  *left_len = non_alpha;

  if (!equal_sign) {
    wlog (LEVEL_FATAL, "config_parse_opt: '%s' has no '='", line);
    exit (EXIT_FAILURE);
  }

  for (size_t i = non_alpha; i < equal_sign; ++i)
    if (!isspace (line[i])) {
      wlog (LEVEL_FATAL, "config_parse_opt: '%s': lvalue contains '%c'", line, line[i]);
      exit (EXIT_FAILURE);
    }

  // Find right option
  *right = 0;
  for (size_t i = equal_sign + 1; i < len; ++i)
    if (line[i] == '\'') {
      *right = i + 1;
      break;
    } else if (!isspace (line[i])) {
      wlog (LEVEL_FATAL, "config_parse_opt: '%s': preceding rvalue is '%c'", line, line[i]);
      exit (EXIT_FAILURE);
    }

  if (!*right) {
    wlog (LEVEL_FATAL, "config_parse_opt: '%s': rvalue not found", line);
    exit (EXIT_FAILURE);
  }

  size_t end_right = 0;
  for (size_t i = *right; i < len; ++i)
    if (line[i] == '\'') {
      end_right = i;
      break;
    }
  if (!end_right) {
    wlog (LEVEL_FATAL, "config_parse_opt: '%s': rvalue no matching `'`", line);
    exit (EXIT_FAILURE);
  }
  *right_len = end_right - *right;
}

/* 1 = an option was read, 0 = no option read, -1 = EOF. free must be called on *free_ptr */
int
config_parse_line (FILE *file, char **left_ptr, char **right_ptr)
{
  char *line = NULL; // tell getline to malloc
  size_t right, right_len, left_len, len;

  errno = 0;
  if (-1 != getline (&line, &len, file)) {
    len = strlen (line);

    // Remove trailing whitespace (e.g. newline)
    while (len > 0 && isspace (line[len - 1]))
      len--;
    line[len] = '\0';
    if (!len)
      return 0;

    // Ignore commented lines
    if (line[0] == '#')
      return 0;

    config_parse_line_opt (line, len, &left_len, &right, &right_len);
    *left_ptr = line;
    *right_ptr = &line[right];

    // Terminate each option
    (*left_ptr)[left_len] = '\0';
    (*right_ptr)[right_len] = '\0';
    return 1;
  }

  // must be called anyways
  if (line)
    free (line);

  if (errno)
    PANIC ("getline");
  return -1;
}

/* 0 on success, -1 on failure */
static int
parse_long (char *s, unsigned long *n, int base)
{
  errno = 0;
  char *stop = NULL;
  *n = strtoul (s, &stop, base);
  if (errno || !stop || stop[0] != '\0')
    return -1;
  return 0;
}

/* loads a config file that overrides the defaults */
void
config_load (char *config_path, struct config *conf)
{
  FILE *fptr;
  char *leftopt, *rightopt;
  int code;

  config_default_values (conf);

  // open config file
  if (!(fptr = fopen (config_path, "r"))) {
    wlog (LEVEL_WARN, "couldn't open '%s' (using defaults): %s", config_path, strerror (errno));
    return;
  }

  while (0 <= (code = config_parse_line (fptr, &leftopt, &rightopt))) {
    if (!code)
      continue;

    if (!strcmp (leftopt, "begin")) {
      unsigned long n;
      if (0 > parse_long (rightopt, &n, 16))
        PANIC ("'begin' is not hexadecimal");
      conf->begin = n;
      ASSERT (conf->begin < conf->end);
    }

    else if (!strcmp (leftopt, "end")) {
      unsigned long n;
      if (0 > parse_long (rightopt, &n, 16))
        PANIC ("'end' is not hexadecimal");
      conf->end = n;
      ASSERT (conf->begin < conf->end);
    }

    else if (!strcmp (leftopt, "logfile")) {
      FILE *fptr = fopen (rightopt, "a");
      ASSERT (fptr);
      log_file = fptr;
    }

    else if (!strcmp (leftopt, "logfilelevel")) {
      unsigned long n;
      if (0 > parse_long (rightopt, &n, 10))
        PANIC ("'logfilelevel' is not decimal");
      #if LEVEL_TRACE != 0
      ASSERT (LEVEL_TRACE <= n);
      #endif
      ASSERT (n <= LEVEL_FATAL);
      log_file_level = n;
    }

    else if (!strcmp (leftopt, "speed")) {
      unsigned long n;
      if (0 > parse_long (rightopt, &n, 10))
        PANIC ("'speed' is not decimal");
      conf->datagrams_per_sec = n;
    }

    else if (!strcmp (leftopt, "numsends")) {
      unsigned long n;
      if (0 > parse_long (rightopt, &n, 10))
        PANIC ("'numsends' is not decimal");
      ASSERT (1 <= n && n <= MAX_REPLIES);
      conf->sends_per_addr = n;
    }

    else {
      wlog (LEVEL_FATAL, "'%s' not an option", leftopt);
      exit (EXIT_FAILURE);
    }

    wlog (LEVEL_INFO, "(config) %s = %s", leftopt, rightopt);

    // this was malloc'd
    free (leftopt);
  }

  fclose (fptr);
}

void
config_default_values (struct config *conf)
{
  conf->begin = 0;
  conf->end = UINT32_MAX;
  conf->datagrams_per_sec = DEFAULT_DATAGRAMS_PER_SEC;
  conf->sends_per_addr = DEFAULT_NUM_SENDS;
}
