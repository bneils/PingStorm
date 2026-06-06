#ifndef PING_H
#define PING_H

#include <stdint.h>
#include <time.h>
#include <sys/epoll.h>
#include <stdbool.h>

#include "types.h"
#include "list.h"

#define PING_FILENAME "ping.dat"
#define PING_MESSAGE "Say cheese!"
#define PING_TIMEOUT 10

#define FITS_IPV4(ip) (ip <= UINT32_MAX)

extern pthread_mutex_t ping_lock;
extern uint8_t *pings;

struct ping_task {
  enum TaskStatus status;
  enum PingReason reason;
  ipaddr addr;
  int sock;
  time_t timeout_end;
  struct list_elem elem;
  struct epoll_event epoll_obj;
};

uint64_t timespec_diff_ms (struct timespec start_time, struct timespec end_time);

char *ip_htos(ipaddr addr);

int ping_send (ipaddr addr);
int ping_task_advance (struct ping_task *task, int epoll_fd, bool timed_out);
void ping_task_init (struct ping_task *task, ipaddr addr);
void ping_task_erase (struct ping_task *t);
void ping_task_done (struct ping_task *task);

void ping_init (void);


#endif
