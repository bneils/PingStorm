#include <semaphore.h>
#include <sys/mman.h>

#include "worker.h"
#include "logging.h"
#include "ping.h"

// This resource is increased when the main thread has collected messages
// from all threads that they are initialized.
static sem_t sem_worker_begin;

// Tells the main thread the current worker has exited initialization.
// Must be called even if the thread is exiting.
static sem_t sem_worker_inited;

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
  throughput.current_time = time (NULL);

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
  cur = find_next_untried (w_args->begin, w_args->end);
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
      cur = find_next_untried(cur + 1, w_args->end);
    } else {
      ping_task_init (task);
      task->epoll_obj.events = EPOLLIN; // TODO??
      task->epoll_obj.data.fd = -1;
    }
  }

  debug ("now polling");
  for (;;) {
    int num_ready = epoll_wait(epoll_fd, event_queue, MAX_EPOLL_EVENTS, (PING_TIMEOUT + 1 * 1000));

    // Check for timeouts
    time_t now = time (NULL);
    while (!list_empty(&tasks_waiting)) {
      // Peek front of list
      struct ping_task *waiting = list_entry (list_head (&tasks_waiting), struct ping_task, elem);
      ASSERT (waiting->addr != 0);
      // Stop if sorted head is in the future
      if (now < waiting->timeout_end)
        break;
      // Maybe renew this ping task, put it somewhere else on the list
      // If the task wasn't replaced it probably means the timeout wasn't long enough
      if (0 == ping_task_look_renew (waiting, &tasks_waiting, &cur, w_args->end, epoll_fd))
        break;
    }

    for (int i = 0; i < num_ready; i++) {
      struct epoll_event event = event_queue[i];
      int sock = event.data.fd;
      // epoll_ctl(2)
      ASSERT (event.events & EPOLLIN)

      // Check up on timed out pings via linked list.
      // Check up on responded pings via event list. When a ping responds, remove from linked list.
      // TODO: fix this garbage
      for (int i = 0; i < WORK_PER_THREAD; ++i) {
        struct ping_task *task = &tasks[i];
        if (task->sock == sock) {
          ping_task_look_renew (task, &tasks_waiting, &cur, w_args->end, epoll_fd);
          break;
        }
      }
		}
    // Documented No-op, but may as well tell to sync
    msync (pings, IPV4_SIZE, MS_ASYNC);
  }
  return NULL;
}
