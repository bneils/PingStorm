#define _GNU_SOURCE

#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include "worker.h"
#include "macros.h"
#include "ping.h"

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

static void register_handler (void);
static void sig_handler (int sig);
static void print_stop_msg (void);
static void sleep_until (struct timespec *t);

/* set by signal handler; informs workers to stop execution. */
static volatile sig_atomic_t stop_working;

static struct {
  int sent_count;
  int retries;
  int done_count;
  int done_reply_counts[MAX_REPLIES];
  time_t current_time;
  pthread_mutex_t lock;
} throughput;

struct sender_task {
  int is_some;
  int num_sent;
  ipaddr addr;
  struct timespec time_next;
};

void
throughput_tick (int num_recv, int num_sent, int retried)
{
  ASSERT (0 <= num_recv && num_recv <= MAX_REPLIES);

  pthread_mutex_lock (&throughput.lock);
  time_t current_time = time (NULL);

  throughput.sent_count += num_sent;
  if (retried) { throughput.retries++; }
  else {
    throughput.done_reply_counts[num_recv]++;
    throughput.done_count++;
  }

  if (current_time - throughput.current_time >= DEBUG_STATS_SECS) {
    float duration = (float)(current_time - throughput.current_time);

    // finished N/sec (N/sec (0 replies), N/sec (NUM_SEND replies)), retried N/sec

    log (LEVEL_INFO, "finished %.1f (%.1f/sec (0) & %.1f/sec (%d)), retried %.1f/sec, total sent %.1f/sec",
      throughput.done_count / duration, throughput.done_reply_counts[0] / duration,
      throughput.done_reply_counts[NUM_SENDS] / duration, NUM_SENDS, throughput.retries / duration,
      throughput.sent_count / duration);

    memset (throughput.done_reply_counts, 0, sizeof (throughput.done_reply_counts));
    throughput.current_time = current_time;
    throughput.retries = 0;
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

/* prints the termination message once if a stop request was detected. */
static void
print_stop_msg (void)
{
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  static int sent_message;
  if (!stop_working)
    return;
  pthread_mutex_lock (&lock);
  if (!sent_message) {
    log (LEVEL_INFO, "Received termination signal (%d). Shutting down...", stop_working);
    sent_message = 1;
  }
  pthread_mutex_unlock (&lock);
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

uint8_t *
mem_find_not_done(uint8_t *p, uint8_t *end)
{
  for (; p < end; ++p)
    if ((*p & P_DONE) == 0)
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
  uint8_t *ptr;

  pthread_mutex_lock (&ping_lock);
  ptr = mem_find_not_done (&pings[addr], endp);
  pthread_mutex_unlock (&ping_lock);

  return (ptr) ? (ipaddrl)(ptr - pings) : 1UL << 32;
}

static void
sig_handler (int sig)
{
  // tell all other worker threads to stop
  stop_working = (sig) ? sig : 1;
}

static void
register_handler (void)
{
  // Register any signals whose default action is to terminate acc. to signal(7)
  // Excluding SIGSTOP and SIGKILL
  const int signals[] = {
    SIGQUIT, /* like SIGTERM. Ctrl+\ */
    SIGTSTP, // put into bg. Ctrl+Z
    SIGINT, // Ctrl+C
    SIGTERM, // kill
  };
  struct sigaction act = { 0 };
  act.sa_handler = sig_handler;
  for (size_t i = 0; i < CLEN (signals); ++i)
    CHECK (0 > sigaction (signals[i], &act, NULL));
}

static void
sleep_until (struct timespec *t)
{
  struct timespec now;
  clock_gettime (CLOCK_MONOTONIC_RAW, &now);
  // Calculate difference between now and wakeup time
  struct timespec diff = {
    .tv_sec = t->tv_sec - now.tv_sec,
    .tv_nsec = t->tv_nsec - now.tv_nsec,
  };
  if (diff.tv_nsec < 0) {
    diff.tv_sec--;
    diff.tv_nsec += (long)1e9;
  }

  if (diff.tv_sec < 0)
    return;

  // Sleep for at least that amount of time
  while (0 > nanosleep (&diff, &diff))
    ;
}

void *
start_sender (void *ptr)
{
  struct sender_task tasks[DATAGRAMS_PER_SEC * PING_TIMEOUT * 2];
  ipaddrl current;
  int sock;

  memset (tasks, 0, sizeof tasks);
  sock = *(int *)ptr;

  current = pings_next_unknown (0, UINT32_MAX);
  if (current > UINT32_MAX) {
    log (LEVEL_INFO, "Nothing to send");
    sem_post (&sem_worker_inited);
    return NULL;
  }

  sem_post (&sem_worker_inited);
  sem_wait (&sem_worker_begin);

  log (LEVEL_INFO, "Send thread started at %s", ip_ntoa (current));

  for (;;) {
    int progress = 0;
    for (size_t i = 0; i < CLEN (tasks); ++i) {
      struct sender_task *t = &tasks[i];

      if (stop_working)
        return NULL;

      if (t->is_some) {
        // This task was given a time it could until finishing
        sleep_until (&t->time_next);

        // Task has no more sends, we mark it as "done"
        if (t->num_sent == NUM_SENDS) {
          pthread_mutex_lock (&ping_lock);
          pings[t->addr] |= P_DONE;
          int num_replies = count_replies (pings[t->addr]);
          if (num_replies > 0) {
            log (LEVEL_TRACE, "%s (%d)", ip_ntoa (t->addr), num_replies);
          }
          pthread_mutex_unlock (&ping_lock);

          // Depending on number of replies, we can choose to invalidate this result and instead retry.
          if (num_replies == 1 && 1 < NUM_SENDS)
            current = t->addr; // Retry the task

          throughput_tick (num_replies, NUM_SENDS, (current == t->addr));
          t->is_some = 0;
        }
      }

      // Create a new task if this object is empty
      // Initialize this for the new address
      if (!t->is_some && current <= UINT32_MAX) {
        t->addr = current;
        t->is_some = 1;
        t->num_sent = 0;
        pthread_mutex_lock (&ping_lock);
        pings[t->addr] = 0;
        pthread_mutex_unlock (&ping_lock);
        current = pings_next_unknown(current + 1, UINT32_MAX);
      }

      // Do not proceed if this task is empty
      if (!t->is_some)
        continue;

      // Send the ping and reset the cooldown
      int seq = t->num_sent;
      while (0 > ping_send (sock, t->addr, seq)) {
        // Not much we can do if this fails besides just wait and retry
        log_source (LEVEL_ERROR, "ping_send");
        sleep (5);
      }

      // Set cooldown and num sent for future task completion
      clock_gettime (CLOCK_MONOTONIC_RAW, &t->time_next);
      t->time_next.tv_sec += PING_TIMEOUT;
      t->num_sent++;
      progress = 1;

      // Wait to match DATAGRAMS_PER_SEC.
      struct timespec ts = { 0 };
      ts.tv_nsec = 1e9 / DATAGRAMS_PER_SEC;
      while (0 > nanosleep (&ts, &ts))
        ;
    }

    // Terminal condition (!progress means no pings were sent)
    if (current > UINT32_MAX && !progress)
      break;
  }
  log (LEVEL_INFO, "Send thread exiting...");

  return NULL;
}

void *
start_receiver (void *ptr)
{
  int sock;
  sock = *(int *)ptr;

  sem_post (&sem_worker_inited);
  sem_wait (&sem_worker_begin);

  log (LEVEL_INFO, "Receive thread started");

  while (!stop_working)
    ping_recv (sock);
  return NULL;
}

/* Create a large number of threads to start working on network requests. */
void
start_workers (void)
{
  pthread_t tids[2];
  int sock;

  // Initially 0 since all workers need permission
  sem_init (&sem_worker_begin, 0, 0);

  // Initially 0 since no worker has initialized yet
  sem_init (&sem_worker_inited, 0, 0);

  ping_init ();
  register_handler ();
  CHECK (0 > (sock = socket_create ()));

  throughput_init ();

  // Ensure that the receiver starts before the sender
  pthread_create (&tids[0], NULL, start_receiver, &sock);
  sem_wait (&sem_worker_inited);
  sem_post (&sem_worker_begin);
  pthread_create (&tids[1], NULL, start_sender, &sock);
  sem_wait (&sem_worker_inited);
  sem_post (&sem_worker_begin);

  // Wait until all threads exit
  struct timespec ts = {
    .tv_sec = 0,
    .tv_nsec = (int)1e9 / 2, // .5s
  };

  for (int i = 0; i < 2; ++i) {
    int err;
    // If the stop message is detected, print it.
    // This is something the main thread will do as it can execute in short intervals,
    // with no long blocking operations (such as epoll_wait).
    while ((err = pthread_timedjoin_np (tids[i], NULL, &ts)))
      print_stop_msg ();
  }
  print_stop_msg ();
}
