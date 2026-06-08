#define _FILE_OFFSET_BITS 64

#include <err.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/resource.h>

#include "logging.h"
#include "list.h"
#include "ping.h"
#include "types.h"
#include "worker.h"
#include "title.h"

#define NUM_FD_REQUIRED (NUM_THREADS * WORK_PER_THREAD * 5 / 4)

// Descriptor for opened ping file
static int ping_fd;

/* Increase file limits of the process to allow for large number of sockets. */
void
set_rlimits (void)
{
  struct rlimit limits;
  if (0 > getrlimit (RLIMIT_NOFILE, &limits))
    err (EXIT_FAILURE, "getrlimit");
  if (limits.rlim_max < NUM_FD_REQUIRED) {
    fprintf(stderr, "The hard file descriptor limit is too low. %lu < %u\n", limits.rlim_max, NUM_FD_REQUIRED);
    exit (1);
  }
  if (limits.rlim_cur < NUM_FD_REQUIRED) {
    limits.rlim_cur = limits.rlim_max;
    if (0 > setrlimit (RLIMIT_NOFILE, &limits))
      err (EXIT_FAILURE, "setrlimit");
    debug ("Set soft limit to %lu", limits.rlim_cur);
  }
}

/* sets ping_fd and backs the `pings` pointer to the file contents. */
void
ping_file_open (void)
{
  // Open ping file for reading/writing
  ping_fd = open (PING_FILENAME, O_RDWR | O_CREAT, (mode_t)0644);
  if (0 > ping_fd)
    err (EXIT_FAILURE, "open");

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
    debug ("Filling P_PRIVATE regions in ping file");

    // Fill the region with P_PRIVATE.
    for (uint32_t i = 0; i < CLEN (special_subnets); ++i) {
      uint32_t network, netmask;
      network = special_subnets[i][0];
      netmask = special_subnets[i][1];
      debug ("%x %x", network, netmask);
      memset (&pings[network], P_PRIVATE, ~netmask + 1);
    }

    msync (pings, IPV4_SIZE, MS_SYNC);
  }
  #ifdef CHECK_RESERVED
  // Verify regions are OK
  debug ("Validating P_PRIVATE regions thoroughly");
  for (uint64_t a = 0; a <= UINT32_MAX; ++a) {
    bool actual = (P_PRIVATE == pings[a]);
    bool expected = is_special (a) ? true : false;
    ASSERT (actual == expected);
  }
  #endif
}

/* Cleanup allocated resources properly before closing. */
void
cleanup (void)
{
  debug ("Cleaning up resources.");
  msync (pings, IPV4_SIZE, MS_SYNC);
  munmap (pings, IPV4_SIZE);
  close (ping_fd);
}

int
main (void)
{
  const char *title, *attr;
  if (0 > get_titlecard(&title, &attr))
    PERROR ("get_titlecard");
  else
    printf ("%s\n%s\n\n%s\n\n", title, attr, TITLE_WEBSITE_ATTRIBUTION);
  set_rlimits ();
  ping_file_open ();
  start_workers ();
  cleanup ();
}
