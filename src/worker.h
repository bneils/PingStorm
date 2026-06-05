#ifndef WORKER_H
#define WORKER_H

#include "types.h"

#define NUM_THREADS 8
#define WORK_PER_THREAD 256
#define MAX_EPOLL_EVENTS 256

struct worker_args {
  ipaddrl begin, end;
  pthread_t tid;
};

void start_workers (void);
void *thread_worker (void *args);

#endif
