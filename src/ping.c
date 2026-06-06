#include <asm-generic/errno.h>
#include <string.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>

#include "ping.h"
#include "types.h"
#include "logging.h"
#include "worker.h"
#include "list.h"

// synchronize access to the mapped file
pthread_mutex_t ping_lock;
uint8_t *pings;

uint64_t
timespec_diff_ms (struct timespec start_time, struct timespec end_time) {
  return (end_time.tv_sec - start_time.tv_sec) * 1e3
    + (end_time.tv_nsec - start_time.tv_nsec) / 1e6;
}

uint16_t
hash_ipaddr (ipaddr addr)
{
  return ((addr & 0xff00) >> 8) ^ (addr & 0xff);
}

/* Try sending an ICMP echo without blocking.
 * Returns 0 on success and -1 if send failed.
 * The function may fail if `wait` is false, but can also fail in other scenarios,
 * so the return code should always be handled by the caller.
 * This function signals system network congestion. */
int
ping_send (int sock, ipaddr addr, bool wait)
{
  // Source: https://sturmflut.github.io/linux/ubuntu/2015/01/17/unprivileged-icmp-sockets-on-linux/
  // This helped me with the socket creation, though I've used my own before.
  struct sockaddr_in sock_addr;
  struct icmphdr icmp_hdr;
  char packetdata[sizeof icmp_hdr];

  CHECK (sock < 0);

  // Initialize the destination address
  memset (&sock_addr, 0, sizeof sock_addr);
  sock_addr.sin_family = AF_INET;
  sock_addr.sin_addr.s_addr = htonl (addr);

  // Initialize the ICMP header
  memset (&icmp_hdr, 0, sizeof icmp_hdr);
  icmp_hdr.type = ICMP_ECHO;
  icmp_hdr.un.echo.id = 0; // TODO: encode IP into this
  icmp_hdr.un.echo.sequence = htons (hash_ipaddr (addr));

  // Initialize the packet data (header and payload)
  memcpy (packetdata, &icmp_hdr, sizeof icmp_hdr);

  // Send the packet; if it fails due to blocking then we retry but with the flag disabled.
  if (0 > sendto (sock, packetdata, sizeof packetdata, (wait) ? 0 : MSG_DONTWAIT, (struct sockaddr *) &sock_addr, sizeof sock_addr)) {
    switch (errno) {
    // The usual culprit: the system warning us it would block (regardless of MSG_DONTWAIT)
    case EWOULDBLOCK:
    #if EAGAIN != EWOULDBLOCK
    case EAGAIN:
    #endif
    // Other possible errors
    case EINTR:
    case ECONNRESET:
    case ENOBUFS:
      errno = 0;
      return -1;
    default:
      PANIC ("Bad");
    }
  }

  return 0;
}

/* Advance a ping task to a different status (like T_DONE or T_SENT).
 * This is called whenever an event arrives for a task, or if a task timed out.
 * If a T_NOTSENT task fails, it will revert to T_NONE.
 * Returns 0 on successful advancement or -1 if it could not advance (T_NONE).
 */
int
ping_task_advance (struct ping_task *task)
{
  if (task->status == T_NONE) return -1;
  if (task->status == T_DONE) return 0;

  if (task->status == T_NOTSENT) {
    // Try to send the ping.
    if (0 > ping_send (task->sock, task->addr, true))
      return -1;
    task->timeout_end = time (NULL) + PING_TIMEOUT;
    task->status = T_SENT;
    return 0;
  }

  // Shouldn't happen
  if (task->status != T_SENT)
    return 0;

  // Handle timed out tasks
  if (time (NULL) >= task->timeout_end) {
    // Skip checking the socket if we know it has timed out
    if (task->reason == P_UNKNOWN) task->reason = P_NOREPLY;
    task->status = T_DONE;
    return 0;
  }

  // Try to read from the socket if it's not timed out
  char recv_buf[256];
  for (;;) {
    if (0 > recv (task->sock, recv_buf, sizeof recv_buf, MSG_DONTWAIT))
      switch (errno) {
      // Retry due to interrupt (curse you unix)
      case EINTR:
        continue;
      // Allow errors due to non-blocking mode
      case EAGAIN:
      #if EAGAIN != EWOULDBLOCK
      case EWOULDBLOCK:
      #endif
      // Can be safely ignored
      case ECONNREFUSED:
      return 0;
      default:
        // Otherwise fatal errors
        PANIC ("recv");
      }
    break;
  }

  // We had a response waiting
  struct icmphdr *icmp_hd = (void *)recv_buf;
  ASSERT (icmp_hd->type == ICMP_ECHOREPLY)
  // Try to check sequence no. / identification / source IP
  if (icmp_hd->un.echo.sequence != htons (hash_ipaddr(task->addr))) {
    debug ("ping address doesn't match: %X != %s", ntohs (icmp_hd->un.echo.sequence), ip_htos (task->addr));
    return 0;
  }

  if (task->reason == P_UNKNOWN) {
    // Don't stop the task if we only get 1 reply
    for (int noretries = 3; noretries > 0; noretries--)
      if (0 <= ping_send (task->sock, task->addr, true))
        goto second_ping_sent;
    PANIC ("failed after 3 tries");
second_ping_sent:
    task->reason = P_REPLIED1;
    task->timeout_end = time (NULL) + PING_TIMEOUT;
  } else if (task->reason == P_REPLIED1) {
    task->reason = P_REPLIED2;
    task->status = T_DONE;
  }
  return 0;
}

/* Initialize a task with a socket and in epoll. Called only once per task object during its lifetime. */
void
ping_task_init (struct ping_task *task, int epoll_fd)
{
  ping_task_erase (task);

  task->sock = socket (AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
  CHECK (0 > task->sock);
  // Increase TTL
  int ttl = UINT8_MAX;
  CHECK (0 > setsockopt (task->sock, IPPROTO_IP, IP_TTL, &ttl, sizeof ttl));

  // Subscribe to events for this file descriptor
  struct epoll_event event;
  event.events = EPOLLIN;
  // This union can only store 1 piece of metadata
  event.data.ptr = task;
  CHECK (0 > epoll_ctl (epoll_fd, EPOLL_CTL_ADD, task->sock, &event));
}

void
ping_task_destroy (struct ping_task *task, int epoll_fd)
{
  CHECK (0 > epoll_ctl(epoll_fd, EPOLL_CTL_DEL, task->sock, NULL));
  CHECK (0 > close (task->sock));
}

/* Assign the ping_task's arguments for the first call to ping_task_advance */
void
ping_task_assign (struct ping_task *task, ipaddr addr)
{
  task->addr = addr;
  task->status = T_NOTSENT;
  task->reason = P_UNKNOWN;
}

/* Takes a finished ping task and cleans up: removes it from the list and writes to disk.
 */
void
ping_task_done (struct ping_task *task, struct list *free_list)
{
  ASSERT (task->status == T_DONE);

  throughput_tick (task->reason);

  // Write to memory-mapped region
  pthread_mutex_lock (&ping_lock);
  pings[task->addr] = task->reason;
  pthread_mutex_unlock (&ping_lock);

  // Remove task from its list
  list_remove (&task->elem);

  // Put them back onto their respective lists
  list_push_back (free_list, &task->elem);

  task->status = T_NONE;
}

/* Gives a string from an IPv4 address. Cannot be used recursively or ephemerally. Thread-safe. */
char *
ip_htos(ipaddr addr)
{
  struct in_addr in_addr = {
    .s_addr = htonl (addr),
  };
  return inet_ntoa (in_addr);
}

/* set the object to an empty / nil state */
void
ping_task_erase (struct ping_task *t)
{
  memset (t, 0, sizeof (*t));
  t->sock = -1;
}

/* Initialize the ping module. */
void
ping_init (void)
{
  // Make the ping lock recursive
  pthread_mutexattr_t attr;
  pthread_mutexattr_init (&attr);
  pthread_mutexattr_settype (&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init (&ping_lock, &attr);
  pthread_mutexattr_destroy (&attr);
}
