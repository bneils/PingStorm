#ifndef CONFIG_H
#define CONFIG_H
#include <stdio.h>
#include <stdint.h>

struct config {
  // addresses
  uint32_t begin;
  uint32_t end;
  // log file and what gets reported to the file
  FILE *log_file;
  int log_file_level;
  // work speed
  int datagrams_per_sec;
  // num pings / addr
  int sends_per_addr;
};

void config_load (char *config_path, struct config *conf);
void config_default_values (struct config *conf);
int config_parse_line (FILE *file, char **left_ptr, char **right_ptr);

#endif
