#ifndef WORKER_H
#define WORKER_H

#include "ping.h"

/* If you set this value too high, then the excess datagrams will likely
 * be dropped by a link along the path (the default gateway).
 * There's no way of detecting this, so it's safer to keep the value conservative.
 * Depending on your router's CPU, control plane (IP) decisions could be MUCH slower
 * than the data plane, so do not set this to your router's upload bandwidth.
 * Furthermore, your ISP's hardware might have a hard limit on how many packets / sec you
 * are allowed.
 *
 * If the number of done addresses / sec with 1 reply is non-zero, then this means the rate of
 * dropped packets is high.
 */
#define DATAGRAMS_PER_SEC 512

// Number of pings to send for each address
// Keep it no greater than NUM_REPLIES
#define NUM_SENDS 3

// Amount of time to wait in-between printing stats
#define DEBUG_STATS_SECS (NUM_SENDS * PING_TIMEOUT)

extern const ipaddr special_subnets[17][2];

void start_workers (void);

ipaddr is_special (ipaddr addr);
ipaddrl skip_special (ipaddrl addr);
uint8_t *mem_find_not_done(uint8_t *p, uint8_t *end);
ipaddrl pings_next_unknown (ipaddrl addr, ipaddrl end);

void throughput_tick (int num_recv, int num_sent, int retried);
void throughput_init (void);

#endif
