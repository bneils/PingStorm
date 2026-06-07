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

static void ping_task_erase (struct ping_task *t);

uint64_t
timespec_diff_ms (struct timespec start_time, struct timespec end_time) {
  return (end_time.tv_sec - start_time.tv_sec) * 1e3
    + (end_time.tv_nsec - start_time.tv_nsec) / 1e6;
}

/* Try sending an ICMP echo without blocking.
 * Returns 0 on success and -1 if send failed.
 * The function may fail if `wait` is false, but can also fail in other scenarios (like network failure).
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

  // Initialize the ICMP header (the rest is filled)
  memset (&icmp_hdr, 0, sizeof icmp_hdr);
  icmp_hdr.type = ICMP_ECHO;

  // Initialize the packet data (header and payload)
  memcpy (packetdata, &icmp_hdr, sizeof icmp_hdr);

  // Send the packet; if it fails due to blocking then we retry but with the flag disabled.
  int send_flag = (wait) ? 0 : MSG_DONTWAIT;
  while (0 > sendto (sock, packetdata, sizeof (packetdata), send_flag,
      (struct sockaddr *) &sock_addr, sizeof sock_addr))
    switch (errno) {
    // If we get interrupted then we should re-try
    case EINTR:
      continue;
    // The usual culprit: the system warning us it would block (regardless of MSG_DONTWAIT)
    case EWOULDBLOCK:
    #if EAGAIN != EWOULDBLOCK
    case EAGAIN:
    #endif
    // System-related issues (like losing connectivity)
    case ECONNRESET:
    case ENOBUFS:
      errno = 0;
      return -1;
    default:
      PANIC ("Bad");
    }

  return 0;
}

/* Updates a task's number of received packets.
 * Will retry on EINTR signal.
 * Returns 0 if the count was incremented, -1 if send failed (OS error), or -2 if the task's address didn't match.
 */
int
ping_task_recv (struct ping_task *task)
{
  // We need to check if the received data matches the task's address.
  // If not, then we should evaluate whether it is for a past task.

  // Try to read from the socket if it's not timed out
  char recv_buf[256];
  struct sockaddr_in sender_saddr;
  in_addr_t sender_addr;
  socklen_t size = sizeof sender_saddr;
  for (;;) {
    if (0 > recvfrom (task->sock, recv_buf, sizeof recv_buf, MSG_DONTWAIT, (struct sockaddr *) &sender_saddr, &size))
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
      return -1;
      default:
        // Otherwise fatal errors
        PANIC ("recv");
      }
    break;
  }

  // Check source IP of received packet.
  sender_addr = ntohl (sender_saddr.sin_addr.s_addr);
  if (sender_addr != task->addr) {
    pthread_mutex_lock (&ping_lock);
    enum PingReason reason = pings[sender_addr];
    if (P_REPLIED_0 <= reason && reason < P_REPLIED_MAX)
      pings[sender_addr] = ++reason;
    pthread_mutex_unlock (&ping_lock);
    debug ("Received address %8x is not %8x: set to %d recv_count", task->addr, sender_addr, reason - P_REPLIED_0);
    return -1;
  }

  task->num_recv++;
  return 0;
}

/* Returns 0 if the task became timed out after this call. Returns -1 if the task is unchanged. */
int
ping_task_timeout (struct ping_task *task)
{
  // Handle timed out tasks
  if (time (NULL) < task->timeout_end)
    return -1;
  return 0;
}

/* Send a task, increment its count, and reset its timeout.
 * If send fails (due to conditions) it returns -1.
 * Returns 0 on success.
 */
int
ping_task_send (struct ping_task *task)
{
  // Try to send the ping.
  if (0 > ping_send (task->sock, task->addr, true))
    return -1;
  task->num_sent++;
  task->timeout_end = time (NULL) + PING_TIMEOUT;
  return 0;
}

/* Initialize a task with a socket and in epoll. Called only once per task object during its lifetime. */
void
ping_task_init (struct ping_task *task, int epoll_fd)
{
  ping_task_erase (task);

  CHECK (0 > (task->sock = socket (AF_INET, SOCK_DGRAM, IPPROTO_ICMP)));

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
  task->num_sent = 0;
  task->num_recv = 0;
}

/* Takes a finished ping task and cleans up: removes it from the list and writes to disk.
 * After this call, the task will be in the free list.
 */
void
ping_task_done (struct ping_task *task, struct list *free_list)
{
  ASSERT (task->num_sent == NUM_SENDS);

  throughput_tick (task->num_recv);

  // Write to memory-mapped region
  pthread_mutex_lock (&ping_lock);
  pings[task->addr] = P_REPLIED_0 + task->num_recv;
  pthread_mutex_unlock (&ping_lock);

  // Remove task from its list
  list_remove (&task->elem);

  // Put them back onto their respective lists
  list_push_back (free_list, &task->elem);
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

/* set the object to an empty / nil state. Should NOT be used by other functions. */
static void
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
