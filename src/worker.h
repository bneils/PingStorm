#ifndef WORKER_H
#define WORKER_H

#include "ping.h"
#include "config.h"

#include <pthread.h>

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
#define DEFAULT_DATAGRAMS_PER_SEC 1000

// Number of pings to send for each address
// Keep it no greater than NUM_REPLIES
#define DEFAULT_NUM_SENDS 3

#define PULSE_SEQ 100
#define PULSE_CHECK_SECS 1
#define PULSE_SAMPLE_SIZE 20
#define PULSE_IP 0x01010101

// Specifies that our sleeps should be in certain sized intervals.
// Selecting a higher intervals means sleeping less often.
#define SLEEP_INTERVAL_MS 50

// The percent to scale the sending rate when a health check fails.
// A value of 0.75 means to cut back 25%
#define HEALTH_FAILED_SCALE 0.75
#define HEALTH_RECOVER_SCALE (1.0 / (HEALTH_FAILED_SCALE * HEALTH_FAILED_SCALE))

#define HEALTH_RECOVERY_STEPS 5

extern int pulse_recv;
extern pthread_mutex_t pulse_lock;
extern const ipaddr special_subnets[17][2];

void start_workers (struct config *conf);

ipaddr is_special (ipaddr addr);
ipaddrl skip_special (ipaddrl addr);
uint8_t *mem_find_not_done(uint8_t *p, uint8_t *end);
ipaddrl pings_next_unknown (ipaddrl addr, ipaddrl end);

void throughput_tick (int num_recv, int num_sent, ipaddr addr_done, struct config *cnf);
void throughput_init (void);

#endif
