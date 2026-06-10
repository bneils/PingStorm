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
#include "config.h"

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

struct sender_args {
  int sock;
  struct config *conf;
};

void
throughput_tick (int num_recv, int num_sent, ipaddr addr_done)
{
  ASSERT (0 <= num_recv && num_recv <= MAX_REPLIES);

  pthread_mutex_lock (&throughput.lock);
  time_t current_time = time (NULL);

  throughput.sent_count += num_sent;
  throughput.done_reply_counts[num_recv]++;
  throughput.done_count++;

  if (current_time - throughput.current_time >= DEBUG_STATS_SECS) {
    float duration = (float)(current_time - throughput.current_time);

    log (LEVEL_INFO,
      "Finished %.1f (%.1f/sec (0), %.1f/sec (1), %.1f/sec (2), %.1f/sec (3)). Total sent %.1f/sec. %s",
      throughput.done_count / duration,
      throughput.done_reply_counts[0] / duration,
      throughput.done_reply_counts[1] / duration,
      throughput.done_reply_counts[2] / duration,
      throughput.done_reply_counts[3] / duration,
      throughput.sent_count / duration,
      ip_ntoa (addr_done)
    );

    memset (throughput.done_reply_counts, 0, sizeof (throughput.done_reply_counts));
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
  struct sender_args *args = ptr;
  struct sender_task *tasks;
  struct config *cnf;
  size_t len;
  ipaddrl current;
  ipaddr end;
  int sock;

  cnf = args->conf;
  len = cnf->datagrams_per_sec * (PING_TIMEOUT + 1);
  tasks = calloc (len, sizeof (struct sender_task));
  sock = args->sock;
  end = cnf->end;

  current = pings_next_unknown (cnf->begin, end);
  if (current > end) {
    log (LEVEL_WARN, "Sender has nothing to send. Exiting...");
    stop_working = 1;
    sem_post (&sem_worker_inited);
    return NULL;
  }

  sem_post (&sem_worker_inited);
  sem_wait (&sem_worker_begin);

  log (LEVEL_INFO, "Send thread started at %s", ip_ntoa (current));

  for (;;) {
    if (stop_working)
      break;
    int progress = 0;
    for (size_t i = 0; i < len; ++i) {
      struct sender_task *t = &tasks[i];

      // Calling break in here will quickly exit outer loop
      if (stop_working)
        break;

      if (t->is_some) {
        // This task was given a time it could until finishing
        sleep_until (&t->time_next);

        // Task has no more sends, we mark it as "done"
        if (t->num_sent == cnf->sends_per_addr) {
          pthread_mutex_lock (&ping_lock);
          pings[t->addr] |= P_DONE;
          int num_replies = count_replies (pings[t->addr], cnf->sends_per_addr);
          pthread_mutex_unlock (&ping_lock);
          throughput_tick (num_replies, cnf->sends_per_addr, t->addr);
          t->is_some = 0;
        }
      }

      // Create a new task if this object is empty
      // Initialize this for the new address
      if (!t->is_some && current <= end) {
        t->addr = current;
        t->is_some = 1;
        t->num_sent = 0;
        pthread_mutex_lock (&ping_lock);
        pings[t->addr] = 0;
        pthread_mutex_unlock (&ping_lock);
        current = pings_next_unknown(current + 1, end);
      }

      // Do not proceed if this task is empty
      if (!t->is_some)
        continue;

      // Send the ping and reset the cooldown
      int seq = t->num_sent;
      while (0 > ping_send (sock, t->addr, seq)) {
        // Not much we can do if this fails besides just wait and retry
        // Main cause seems to be "Operation not permitted" and it's semi-frequent (for my VPS)
        log_source (LEVEL_ERROR, "ping_send");
        sleep (1);
      }

      // Set cooldown and num sent for future task completion
      clock_gettime (CLOCK_MONOTONIC_RAW, &t->time_next);
      t->time_next.tv_sec += PING_TIMEOUT;
      t->num_sent++;
      progress = 1;

      // Wait to match DATAGRAMS_PER_SEC.
      struct timespec ts = { 0 };
      ts.tv_nsec = (int)1e9 / cnf->datagrams_per_sec;
      while (0 > nanosleep (&ts, &ts))
        ;
    }

    // Terminal condition (!progress means no pings were sent)
    if (current > end && !progress)
      break;
  }
  log (LEVEL_INFO, "Send thread exiting...");
  stop_working = 1;

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
    if (0 > ping_recv (sock)) {
      log (LEVEL_TRACE, "ping_recv: timeout");
    }

  log (LEVEL_INFO, "Receive thread exiting...");
  return NULL;
}

/* Create a large number of threads to start working on network requests. */
void
start_workers (struct config *conf)
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

  // Provide sender with arguments
  struct sender_args args = {
    .sock = sock,
    .conf = conf,
  };
  pthread_create (&tids[1], NULL, start_sender, &args);
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
