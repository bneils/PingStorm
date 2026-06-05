#ifndef PING_H
#define PING_H

#include <stdint.h>
#include <time.h>
#include <sys/epoll.h>

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

char *ip_htos(ipaddr addr);
int ping_send (ipaddr addr);
enum PingReason ping_task (struct ping_task *task, int epoll_fd);
void ping_task_start_new (struct ping_task *task, ipaddr cur, int epoll_fd);
void ping_task_init (struct ping_task *t);
void ping_init (void);

int ping_task_look_renew (
  struct ping_task *task,
  struct list *tasks_waiting,
  ipaddrl *cur, ipaddrl end,
  int epoll_fd
);


#endif
