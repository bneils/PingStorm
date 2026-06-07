#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <pthread.h>

#define CLEN(arr) (sizeof (arr) / sizeof (*arr))
#define IPV4_SIZE (1UL << 32)

typedef uint32_t ipaddr;
typedef uint64_t ipaddrl;

enum TaskStatus {
  T_NONE = 0,       /* The task is inactive and not used for anything */
  T_NOTSENT = 1,    /* The task hasn't sent any data and isn't expecting a response. */
  T_SENT = 2,       /* The task has data in transit and is waiting for a response. */
  T_DONE = 3,       /* The task has finished and can be written to disk. */
};

/* P_REPLIED are sequential. */
enum PingReason {
  P_UNKNOWN = 0,    /* We haven't reached out to the address. (Must be zero). */
  P_PRIVATE = 1,    /* The address is private, reserved, and not routable. */
  P_REPLIED_0 = 0xF,  /* The address replied 0 times. */
  P_REPLIED_MAX = P_REPLIED_0 + 6,  /* The address replied MAX times. */
};

#define MAX_REPLIES (P_REPLIED_MAX - P_REPLIED_0 + 1)

/* The shared buffer holding all ping results */
extern uint8_t *pings;
extern pthread_mutex_t ping_lock;

#endif
