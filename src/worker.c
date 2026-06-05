#include <semaphore.h>
#include <sys/mman.h>
#include <time.h>
#include <stdlib.h>

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
  int num_pings;
  int num_success;
  int num_failed;
  time_t current_time;
  pthread_mutex_t lock;
} throughput;

/* waits a very short amount of time to allow synchronized threads to drift */
static void
sleep_drift (void)
{
  struct timespec ts, rem;
  long r = random ();

  // 1-10 ms drift
  ts.tv_nsec = r % (long)1e7;
  ts.tv_sec = 0;

  while (0 > nanosleep (&ts, &rem) && errno == EINTR)
    ts = rem;
}

void
throughput_tick (enum PingReason reason)
{
  pthread_mutex_lock (&throughput.lock);
  time_t current_time = time (NULL);
  throughput.num_pings++;
  if (reason == P_NOREPLY) throughput.num_failed++;
  else if (reason == P_REPLIED) throughput.num_success++;

  if (current_time - throughput.current_time >= 10) {
    int duration = (int)(current_time - throughput.current_time);
    debug ("%d timed out/sec, %d replied/sec (%d%%), total %d/sec",
      throughput.num_failed / duration,
      throughput.num_success / duration,
      (throughput.num_pings) ? throughput.num_success * 100 / throughput.num_pings : 0,
      throughput.num_pings / duration
    );
    throughput.current_time = current_time;
    throughput.num_pings = 0;
    throughput.num_failed = 0;
    throughput.num_success = 0;
  }
  pthread_mutex_unlock (&throughput.lock);
}

void
throughput_init (void)
{
  throughput.current_time = time (NULL);
  throughput.num_failed = 0;
  throughput.num_pings = 0;
  throughput.num_success = 0;
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
    sem_wait(&sem_worker_inited);

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
  struct worker_args *w_args = args;
  struct ping_task tasks[WORK_PER_THREAD];
  struct epoll_event event_queue[MAX_EPOLL_EVENTS];
  struct list tasks_waiting; // ordered by timeout
  int epoll_fd;
  ipaddrl cur;

  list_init (&tasks_waiting);

  // Pick starting point (which might take a while)
  cur = pings_next_unknown (w_args->begin, w_args->end);
  if (cur > UINT32_MAX) {
    debug ("P_UNKNOWN not found in range %lX-%lX", w_args->begin, w_args->end);
    sem_post (&sem_worker_inited);
    return NULL;
  }
  debug ("Starting at %-15s (%8lX, %8lX)", ip_htos (cur), w_args->begin, w_args->end);
  // Post when you've found your starting point
  sem_post (&sem_worker_inited);
  // Wait for main thread to signal readiness
  sem_wait (&sem_worker_begin);

  // Create an epoll object
  CHECK (0 > (epoll_fd = epoll_create1 (0)));

  // Initialize the work queue `tasks` with outgoing pings
  for (int i = 0; i < WORK_PER_THREAD; ++i) {
    struct ping_task *task = &tasks[i];
    if (cur <= UINT32_MAX) {
      // Start the ping task
      ping_task_start_new (task, cur, epoll_fd);
      list_push_back (&tasks_waiting, &task->elem);
      // Advance the address cursor
      cur = pings_next_unknown(cur + 1, w_args->end);
    } else {
      ping_task_init (task);
      task->epoll_obj.events = EPOLLIN; // TODO??
      task->epoll_obj.data.fd = -1;
    }
  }

  while (!list_empty (&tasks_waiting)) {
    int num_ready = epoll_wait (epoll_fd, event_queue, MAX_EPOLL_EVENTS, (PING_TIMEOUT + 1) * 1000);
    sleep_drift ();

    // Handle received events
    for (int i = 0; i < num_ready; i++) {
      struct epoll_event event = event_queue[i];

      // epoll_ctl(2)
      ASSERT (event.events & EPOLLIN)

      struct ping_task *task = event.data.ptr;
      ping_task_look_renew (task, &tasks_waiting, &cur, w_args->end, epoll_fd);
		}

    // Check for timeouts via the linked list
    time_t now = time (NULL);
    while (!list_empty(&tasks_waiting)) {
      // Peek front of list
      struct ping_task *waiting = list_entry (list_front (&tasks_waiting), struct ping_task, elem);
      ASSERT (waiting->addr != 0);
      // Stop if sorted head is in the future
      if (now < waiting->timeout_end)
        break;
      // Maybe renew this ping task, put it somewhere else on the list
      // If the task wasn't replaced it probably means the timeout wasn't long enough
      if (0 == ping_task_look_renew (waiting, &tasks_waiting, &cur, w_args->end, epoll_fd))
        break;
    }

    // Documented No-op, but may as well tell to sync
    msync (pings, IPV4_SIZE, MS_ASYNC);
  }
  debug ("Finished work load");
  return NULL;
}
