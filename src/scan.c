#define _FILE_OFFSET_BITS 64

#include <err.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

#include <sys/mman.h>
#include <sys/resource.h>

#include "macros.h"
#include "ping.h"
#include "worker.h"
#include "title.h"

// Descriptor for opened ping file
static int ping_fd;

static void print_usage (void);
static void ping_file_open (void);
static void cleanup (void);

/* sets ping_fd and backs the `pings` pointer to the file contents. */
static void
ping_file_open (void)
{
  // Open ping file for reading/writing
  CHECK (0 > (ping_fd = open (PING_FILENAME, O_RDWR | O_CREAT, (mode_t)0644)));

  // Go to end of file
  lseek (ping_fd, IPV4_SIZE - 1, SEEK_SET);

  // If nothing read, stretch the file
  char c = '\0';
  int init_file = 0;
  if (0 == read (ping_fd, &c, 1)) {
    if (0 > write (ping_fd, &c, 1)) {
      close (ping_fd);
      err (EXIT_FAILURE, "write");
    }

    init_file = 1;
  }

  pings = mmap (NULL, IPV4_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, ping_fd, 0);
  if (pings == MAP_FAILED) {
    close (ping_fd);
    err (EXIT_FAILURE, "mmap");
  }

  if (init_file) {
    // Fill the private memory regions
    log (LEVEL_INFO, "Filling reserved regions in ping file");

    // Fill the region with P_PRIVATE.
    for (uint32_t i = 0; i < CLEN (special_subnets); ++i) {
      uint32_t network, netmask;
      network = special_subnets[i][0];
      netmask = special_subnets[i][1];
      memset (&pings[network], P_PRIVATE | P_DONE, ~netmask + 1);
    }

    msync (pings, IPV4_SIZE, MS_SYNC);
  }
  #ifdef CHECK_RESERVED
  // Verify regions are OK
  log (LEVEL_INFO, "Validating reserved regions thoroughly");
  for (uint64_t a = 0; a <= UINT32_MAX; ++a) {
    bool actual = (P_PRIVATE & pings[a]) != 0;
    bool expected = is_special (a) != 0;
    ASSERT (actual == expected);
  }
  #endif
}

/* Cleanup allocated resources properly before closing. */
static void
cleanup (void)
{
  log (LEVEL_INFO, "Cleaning up resources.");
  msync (pings, IPV4_SIZE, MS_SYNC);
  munmap (pings, IPV4_SIZE);
  close (ping_fd);
}

static void
print_usage (void)
{
  log (LEVEL_FATAL, "usage: storm [<start (hex)> <end (hex)>]");
}

int
main (int argc, char *argv[])
{
  unsigned long start, end;
  const char *title, *attr;
  if (0 > get_titlecard(&title, &attr))
    log_source (LEVEL_ERROR, "get_titlecard");
  else
    printf ("%s\nPing Storm by Ben Neilsen\n\n%s\n%s\n\n", title, attr, TITLE_WEBSITE_ATTRIBUTION);

  // Grab address range
  if (argc == 1) {
    // Default
    start = 0;
    end = UINT32_MAX;
  } else if (argc == 3) {
    // Custom
    errno = 0;
    char *stop1, *stop2;
    stop1 = stop2 = NULL;
    start = strtoul (argv[1], &stop1, 16);
    end = strtoul (argv[2], &stop2, 16);
    if (errno || !stop1 || !stop2 || stop1[0] != '\0'
      || stop2[0] != '\0') {
      print_usage ();
      PANIC ("arguments may have non-hex characters");
    }
    CHECK (start > UINT32_MAX || end > UINT32_MAX);
  } else {
    print_usage ();
    exit (EXIT_FAILURE);
  }

  log (LEVEL_INFO, "Arguments (%d): %8lX - %8lX", argc, start, end);

  ping_file_open ();
  start_workers (start, end);
  cleanup ();
}
