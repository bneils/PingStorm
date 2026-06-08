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

#define MIN_U8_SHIFT 2
#define MAX_U8_SHIFT 7

/* P_REPLIED are sequential. */
enum PingReason {
  P_UNKNOWN = 0,    /* We haven't reached out to the address. (Must be zero). */
  P_DONE = 1,       /* This ping record is complete. */
  P_PRIVATE = 2,    /* The address is private, reserved, and not routable. */
  P_SEQ_0_REPLY = 1 << MIN_U8_SHIFT,  /* The address replied 0 times. */
  P_SEQ_MAX_REPLY = 1 << MAX_U8_SHIFT,  /* The address replied MAX times. */
};

#define MIN_SEQ 0
#define MAX_SEQ (MAX_U8_SHIFT - MIN_U8_SHIFT)
#define MAX_REPLIES (MAX_U8_SHIFT - MIN_U8_SHIFT + 1)

// Perform a bounds check before using this.
#define SEQ_TO_PINGREASON(seq) (1 << ((seq) + MIN_U8_SHIFT))

/* The shared buffer holding all ping results */
extern uint8_t *pings;
extern pthread_mutex_t ping_lock;

#endif
