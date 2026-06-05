#ifndef WORKER_H
#define WORKER_H

#include "types.h"

#define NUM_THREADS 8
#define WORK_PER_THREAD (8 * 1024)

// Conservative 20% response rate (<10 is typical)
#define MAX_EPOLL_EVENTS (WORK_PER_THREAD / 5)

struct worker_args {
  ipaddrl begin, end;
  pthread_t tid;
};

extern const ipaddr special_subnets[17][2];

void start_workers (void);
void *thread_worker (void *args);

ipaddr is_special (ipaddr addr);
ipaddrl skip_special (ipaddrl addr);
void *mem_find (uint8_t *p, uint8_t *end, uint8_t value);
ipaddrl pings_next_unknown (ipaddrl addr, ipaddrl end);

void throughput_tick (enum PingReason reason);
void throughput_init (void);

#endif
