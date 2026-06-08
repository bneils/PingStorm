#include <semaphore.h>
#include <sys/mman.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>

#include "worker.h"
#include "list.h"
#include "logging.h"
#include "ping.h"
#include "types.h"

// This resource is increased when the main thread has collected messages
// from all threads that they are initialized.
static sem_t sem_worker_begin;

// Tells the main thread the current worker has exited initialization.
// Must be called even if the thread is exiting.
static sem_t sem_worker_inited;

// https://en.wikipedia.org/wiki/IPv4#Special-use_addresses
const ipaddr special_subnets[17][2] = {
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

static struct {
  int sent_count;
  int done_count;
  int reply_counts[MAX_REPLIES];
  time_t current_time;
  pthread_mutex_t lock;
} throughput;

void
throughput_tick (int num_recv)
{
  ASSERT (0 <= num_recv && num_recv <= MAX_REPLIES);

  pthread_mutex_lock (&throughput.lock);
  time_t current_time = time (NULL);

  throughput.sent_count += NUM_SENDS;
  throughput.reply_counts[num_recv]++;
  throughput.done_count++;

  if (current_time - throughput.current_time >= DEBUG_STATS_SECS) {
    int duration = (int)(current_time - throughput.current_time);
    // Format the debug string
    char dst[256];
    int dst_size = sizeof (dst);
    int num_written;
    int pos = 0;
    memset (dst, 0, dst_size);
    for (int i = 0; i <= NUM_SENDS; ++i) {
      num_written = snprintf (&dst[pos], dst_size, "%.1f/sec (%d replies), ",
        (float)throughput.reply_counts[i] / duration, i);
      ASSERT (num_written >= 0);
      dst_size -= num_written;
      pos += num_written;
      ASSERT (dst_size >= 0);
    }
    // Add number of sends & finished
    CHECK (0 > snprintf (&dst[pos], dst_size, "sent %.1f/sec, done %.1f/sec",
      (float)throughput.sent_count / duration, (float)throughput.done_count / duration));

    // Output it to terminal
    debugstr (dst);

    memset (throughput.reply_counts, 0, sizeof (throughput.reply_counts));
    throughput.current_time = current_time;
    throughput.sent_count = 0;
    throughput.done_count = 0;
  }
  pthread_mutex_unlock (&throughput.lock);
}

void
throughput_init (void)
{
  throughput.current_time = time (NULL);
  pthread_mutex_init (&throughput.lock, NULL);
}

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
pings_next_unknown (ipaddrl addr, ipaddrl end)
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

/* Create a large number of threads to start working on network requests. */
void
start_workers (void)
{
  struct worker_args w_args[NUM_THREADS];
  pthread_t tids[NUM_THREADS];
  size_t addrs_per_thread = (1ULL << 32) / NUM_THREADS;
  ipaddr start = 0;

  // Initially 0 since all workers need permission
  sem_init (&sem_worker_begin, 0, 0);

  // Initially 0 since no worker has initialized yet
  sem_init (&sem_worker_inited, 0, 0);

  ping_init ();

  for (int i = 0; i < NUM_THREADS; ++i) {
    struct worker_args *arg = &w_args[i];
    arg->begin = start;
    // Make sure the last thread has no off-by-one error or overflow in the stop address.
    arg->end = (i < NUM_THREADS - 1) ? start + addrs_per_thread - 1 : UINT32_MAX;
    start += addrs_per_thread;
    pthread_create (&arg->tid, NULL, thread_worker, arg);
    tids[i] = arg->tid;
  }

  // Wait for all workers to post the inited thread.
  for (int i = 0; i < NUM_THREADS; ++i)
    sem_wait (&sem_worker_inited);

  debug ("All threads ready");
  throughput_init ();

  // Let all workers leave initialization phase
  for (int i = 0; i < NUM_THREADS; ++i)
    sem_post (&sem_worker_begin);

  // Wait until all threads exit
  for (int i = 0; i < NUM_THREADS; ++i)
    pthread_join (tids[i], NULL);
}

void *
thread_worker (void *args)
{
  struct worker_args *w = args;
  struct ping_task tasks[WORK_PER_THREAD];
  struct epoll_event event_queue[MAX_EPOLL_EVENTS];
  struct list timeout_list, free_list, send_list;
  int epoll_fd;
  ipaddrl cur;

  // send list is the list of tasks that need to be sent AGAIN by our worker.
  // it is comprised of tasks that have been sent at least once and need to be sent again.
  // think of it as the "chore" list.
  list_init (&send_list);

  // timeout list is checked for timeouts, consists of tasks in transit.
  list_init (&timeout_list);

  // free list is list of tasks not being used for anything.
  list_init (&free_list);

  ASSERT (WORK_PER_THREAD > 0);

  char begin_addr_s[32];
  char end_addr_s[32];
  strncpy (begin_addr_s, ip_ntoa (w->begin), sizeof (begin_addr_s));
  strncpy (end_addr_s, ip_ntoa (w->end), sizeof (end_addr_s));

  // Pick starting point (which might take a while)
  cur = pings_next_unknown (w->begin, w->end);
  if (cur > UINT32_MAX) {
    debug ("Work not found in (%s, %s)", begin_addr_s, end_addr_s);
    sem_post (&sem_worker_inited);
    return NULL;
  }
  debug ("Starting at %-15s (%s, %s)", ip_ntoa (cur), begin_addr_s, end_addr_s);
  // Post when you've found your starting point
  sem_post (&sem_worker_inited);
  // Wait for main thread to signal readiness
  sem_wait (&sem_worker_begin);

  // Create an epoll object
  CHECK (0 > (epoll_fd = epoll_create1 (0)));

  // Fill the dead list which can be drawn from to create new tasks.
  for (int i = 0; i < WORK_PER_THREAD; ++i) {
    ping_task_init (&tasks[i], epoll_fd);
    list_push_back (&free_list, &tasks[i].elem);
  }

  while (cur <= UINT32_MAX) {
    // Print some debug about the task lists
    if (list_empty (&timeout_list) && !list_empty (&send_list)) debug ("no tasks in transit (send list not empty)");
    else if (list_empty (&timeout_list) && list_empty (&send_list)) debug ("no work in queue");

    // The returned events might be for active or free tasks.
    int num_ready = epoll_wait (epoll_fd, event_queue, MAX_EPOLL_EVENTS, (PING_TIMEOUT + 1) * 1000);

    // Handle received events
    for (int i = 0; i < num_ready; i++) {
      struct epoll_event event = event_queue[i];

      // epoll_ctl(2)
      ASSERT (event.events & EPOLLIN)

      struct ping_task *task = event.data.ptr;

      // Read the failure codes for task_recv,
      // -1 if recv failed (which is probably an OS error)
      // -2 if it was for a different task -- which means
      // we are operating on an undefined object (could be in free / send lists)
      if (0 > ping_task_recv (task))
        continue;

      // Task finished.
      if (task->num_sent == NUM_SENDS) {
        ping_task_done (task, &free_list);
        continue;
      }

      // Move the task to the send list
      list_remove (&task->elem);
      list_push_back (&send_list, &task->elem);
		}

    // Check for timeouts
    while (!list_empty (&timeout_list)) {
      // Peek front of list
      struct ping_task *task = list_entry (list_front (&timeout_list), struct ping_task, elem);

      if (0 > ping_task_timeout (task))
        break;

      if (task->num_sent == NUM_SENDS) {
        ping_task_done (task, &free_list);
        continue;
      }

      // Move the task to the send list
      list_remove (&task->elem);
      list_push_back (&send_list, &task->elem);
    }

    struct timespec start_time, end_time;
    clock_gettime (CLOCK_MONOTONIC_RAW, &start_time);

    // Send the tasks in the send list.
    int failed = 0;
    while (!list_empty (&send_list)) {
      struct ping_task *task = list_entry (list_front (&send_list), struct ping_task, elem);
      // If this fails, we should stop
      if (0 > ping_task_send (task)) {
        failed = 1;
        break;
      }
      // Send succeeded, now we are waiting for a response
      list_remove (&task->elem);
      list_push_back (&timeout_list, &task->elem);
    }
    // Don't proceed further, if the tasks in the send list failed (which are higher priority)
    // then we shouldn't try sending more.
    if (failed)
      continue;

    // Add as many tasks as you can in the active list.
    // Skip drawing from the free list if we have no more tasks to make.
    while (cur <= UINT32_MAX && !list_empty (&free_list)) {
      // Try initiating the next task
      struct ping_task *task = list_entry (list_front (&free_list), struct ping_task, elem);
      ping_task_assign (task, cur);

      // Stop if it fails
      if (0 > ping_task_send (task))
        break;

      // If successful, we move it from the free list to the active list, then getting the next address.
      list_remove (&task->elem);
      list_push_back (&timeout_list, &task->elem);
      cur = pings_next_unknown (cur + 1, w->end);

      // Only spend so much time in this loop.
      // These few lines actually reduce CPU usage and increase throughput dramatically.
      // It had to be placed intentionally because my CPU was hitting 100% and my computer would freeze.
      clock_gettime (CLOCK_MONOTONIC_RAW, &end_time);
      uint64_t delta_ms = timespec_diff_ms (start_time, end_time);
      if (delta_ms >= PING_TIMEOUT * 1000 / 2)
        break;
    }
  }
  // Free the opened sockets
  for (size_t i = 0; i < CLEN (tasks); ++i)
    ping_task_destroy (&tasks[i], epoll_fd);
  debug ("Finished work load");
  return NULL;
}
