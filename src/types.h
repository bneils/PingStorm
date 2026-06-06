#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <pthread.h>

#define CLEN(arr) (sizeof (arr) / sizeof (*arr))
#define IPV4_SIZE (1UL << 32)

typedef uint32_t ipaddr;
typedef uint64_t ipaddrl;

enum TaskStatus {
  T_NONE = 0,
  T_NOTSENT = 1,
  T_SENT = 2,
  T_DONE = 3,
};

enum PingReason : uint8_t {
  P_UNKNOWN = 0,
  P_NOREPLY = 1,
  P_REPLIED1 = 2,
  P_REPLIED2 = 3,
  P_PRIVATE = 4,
};

/* The shared buffer holding all ping results */
extern uint8_t *pings;
extern pthread_mutex_t ping_lock;

#endif
