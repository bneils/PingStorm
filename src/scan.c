#define _FILE_OFFSET_BITS 64

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <err.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <poll.h>
#include <stdbool.h>
#include <semaphore.h>

#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "logging.h"
#include "list.h"
#include "ping.h"
#include "types.h"
#include "worker.h"

#define NUM_FD_REQUIRED (NUM_THREADS * WORK_PER_THREAD * 5 / 4)

struct {
  int num_pings;
  time_t current_time;
  pthread_mutex_t lock;
} throughput = {0, 0, PTHREAD_MUTEX_INITIALIZER};

// Descriptor for opened ping file
static int ping_fd;

// https://en.wikipedia.org/wiki/IPv4#Special-use_addresses
static const ipaddr special_subnets[][2] = {
  {0xE0000000, 0xF0000000}, // 224.0.0.0/4
  {0xF0000000, 0xF0000000}, // 240.0.0.0/4
  {0x00000000, 0xFF000000}, // 0.0.0.0/8
  {0x0A000000, 0xFF000000}, // 10.0.0.0/8
  {0x7F000000, 0xFF000000}, // 127.0.0.0/8
  {0x64400000, 0xFFC00000}, // 100.64.0.0/10
  {0xAC100000, 0xFFF00000}, // 172.16.0.0/12
  {0xC6120000, 0xFFFE0000}, // 198.18.0.0/15
  {0xA9FE0000, 0xFFFF0000}, // 169.254.0.0/16
  {0xC0A80000, 0xFFFF0000}, // 192.168.0.0/16
  {0xC0000000, 0xFFFFFF00}, // 192.0.0.0/24
  {0xC0000200, 0xFFFFFF00}, // 192.0.2.0/24
  {0xC0586300, 0xFFFFFF00}, // 192.88.99.0/24
  {0xC6336400, 0xFFFFFF00}, // 198.51.100.0/24
  {0xCB007100, 0xFFFFFF00}, // 203.0.113.0/24
  {0xE9FC0000, 0xFFFFFF00}, // 233.252.0.0/24
  {0xFFFFFFFF, 0xFFFFFFFF}, // 255.255.255.255/32
};

/* Returns 0 for public addresses, otherwise return the netmask */
ipaddr
is_special (ipaddr addr)
{
  for (size_t i = 0; i < CLEN (special_subnets); ++i) {
    ipaddr network = special_subnets[i][0];
    ipaddr netmask = special_subnets[i][1];
    if ((addr & netmask) == network)
      return netmask;
  }
  return 0;
}



/* Jumps to the next address not located in a "special" subnet.
 * May exceed UINT32_MAX. After this call, is_special always returns false.
 */
ipaddrl
skip_special (ipaddrl addr)
{
  ipaddr netmask;
  while ((netmask = is_special (addr)) && addr <= UINT32_MAX)
    addr = (addr | ~netmask) + 1;
  return addr;
}

uint64_t
timespec_diff_ms (struct timespec start_time, struct timespec end_time) {
  return (end_time.tv_sec - start_time.tv_sec) * 1e3
    + (end_time.tv_nsec - start_time.tv_nsec) / 1e6;
}


void *
mem_find (uint8_t *p, uint8_t *end, uint8_t value)
{
  for (; p < end; ++p)
    if (*p == value)
      return p;
  return NULL;
}

/* Returns next IPv4 address, or if no address is available, will exceed UINT32_MAX */
ipaddrl
find_next_untried (ipaddrl addr, ipaddrl end)
{
  if (addr > UINT32_MAX)
    return addr;

  void *endp = &pings[end + 1];

  pthread_mutex_lock (&ping_lock);
  while (pings[addr] != P_UNKNOWN) {
    uint8_t *p = mem_find (&pings[addr], endp, P_UNKNOWN);
    // not found in current range.
    if (!p) {
      addr = 1UL << 32;
      break;
    }
    // avoid landing in special ranges (e.g multicast)
    // if this region puts us past our `end` then we should stop here
    addr = skip_special (p - pings);
    if (addr > end) {
      addr = 1UL << 32;
      break;
    }
  }
  pthread_mutex_unlock (&ping_lock);

  return addr;
}

void
throughput_tick (void)
{
  pthread_mutex_lock (&throughput.lock);
  time_t current_time = time (NULL);
  throughput.num_pings++;
  if (current_time - throughput.current_time >= 10) {
    debug ("%d pings / sec", throughput.num_pings / (int)(current_time - throughput.current_time));
    throughput.current_time = current_time;
    throughput.num_pings = 0;
  }
  pthread_mutex_unlock (&throughput.lock);
}

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

    // Verify regions are OK
    debug ("Validating P_PRIVATE regions thoroughly");
    for (uint64_t a = 0; a <= UINT32_MAX; ++a) {
      bool actual = (P_PRIVATE == pings[a]);
      bool expected = is_special (a) ? true : false;
      ASSERT (actual == expected);
    }
  }
}

/* Cleanup allocated resources properly before closing. */
void
cleanup (void)
{
  debug ("closing properly...");
  msync (pings, IPV4_SIZE, MS_SYNC);
  munmap (pings, IPV4_SIZE);
  close (ping_fd);
}


int
main (void)
{
  set_rlimits ();
  start_workers ();
  cleanup ();
}
