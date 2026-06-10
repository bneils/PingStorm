#ifndef PING_H
#define PING_H

#include <stdint.h>
#include <time.h>
#include <pthread.h>

#define PING_FILENAME "ping.dat"

// There is no reason to set PING_TIMEOUT low.
// Increasing it will only increase memory usage slightly. (500K -> 1000K)
#define PING_TIMEOUT 10

#define MIN_U8_SHIFT 2
#define MAX_U8_SHIFT 7

#define MIN_SEQ 0
#define MAX_SEQ (MAX_U8_SHIFT - MIN_U8_SHIFT)
#define MAX_REPLIES (MAX_U8_SHIFT - MIN_U8_SHIFT + 1)

#define IPV4_SIZE (1ULL << 32)

#define P_DONE     1       /* This ping is complete. */
#define P_PRIVATE  2       /* The address is private, reserved, and not routable. */

// Perform a bounds check before using this.
#define SEQ_TO_BIT(seq) (1 << ((seq) + MIN_U8_SHIFT))

extern pthread_mutex_t ping_lock;
extern uint8_t *pings;

typedef uint32_t ipaddr;
typedef uint64_t ipaddrl;

char *ip_ntoa (ipaddr addr);

int ping_send (int sock, ipaddr addr, int seq);
int ping_recv (int sock);
int socket_create (void);
int count_replies (int bits, int num_sends);

void ping_init (void);

#endif
