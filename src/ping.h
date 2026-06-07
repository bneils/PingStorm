#ifndef PING_H
#define PING_H

#include <stdint.h>
#include <time.h>
#include <sys/epoll.h>
#include <stdbool.h>

#include "types.h"
#include "list.h"

#define PING_FILENAME "ping.dat"
#define PING_TIMEOUT 10

#define FITS_IPV4(ip) (ip <= UINT32_MAX)

extern pthread_mutex_t ping_lock;
extern uint8_t *pings;

struct ping_task {
  int num_sent;
  int num_recv;
  ipaddr addr;
  int sock;
  time_t timeout_end;
  struct list_elem elem;
};

uint64_t timespec_diff_ms (struct timespec start_time, struct timespec end_time);

char *ip_htos(ipaddr addr);

int ping_send (int sock, ipaddr addr, bool wait);
void ping_task_done (struct ping_task *task, struct list *free_list);
void ping_task_init (struct ping_task *task, int epoll_fd);
void ping_task_destroy (struct ping_task *task, int epoll_fd);
void ping_task_assign (struct ping_task *task, ipaddr addr);

int ping_task_recv (struct ping_task *task);
int ping_task_timeout (struct ping_task *task);
int ping_task_send (struct ping_task *task);

void ping_init (void);

#endif
