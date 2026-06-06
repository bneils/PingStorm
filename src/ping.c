#include <asm-generic/errno.h>
#include <string.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ping.h"
#include "types.h"
#include "logging.h"
#include "worker.h"

// synchronize access to the mapped file
pthread_mutex_t ping_lock;
uint8_t *pings;

uint64_t
timespec_diff_ms (struct timespec start_time, struct timespec end_time) {
  return (end_time.tv_sec - start_time.tv_sec) * 1e3
    + (end_time.tv_nsec - start_time.tv_nsec) / 1e6;
}

/* Try sending an ICMP echo without blocking.
 * Returns socket file descriptor on success and -1 if it would block.
 * This function signals system network congestion. */
int
ping_send (ipaddr addr)
{
  // Source: https://sturmflut.github.io/linux/ubuntu/2015/01/17/unprivileged-icmp-sockets-on-linux/
  // This helped me with the socket creation, though I've used my own before.
  struct sockaddr_in sock_addr;
  struct icmphdr icmp_hdr;
  char packetdata[sizeof icmp_hdr + sizeof PING_MESSAGE];

  // Create a datagram ICMP socket
  int sock = socket (AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
  CHECK (sock < 0);

  // Initialize the destination address
  memset (&sock_addr, 0, sizeof sock_addr);
  sock_addr.sin_family = AF_INET;
  sock_addr.sin_addr.s_addr = htonl (addr);

  // Initialize the ICMP header
  memset (&icmp_hdr, 0, sizeof icmp_hdr);
  icmp_hdr.type = ICMP_ECHO;
  icmp_hdr.un.echo.id = 0; // ignored
  icmp_hdr.un.echo.sequence = 1;

  // Initialize the packet data (header and payload)
  memcpy (packetdata, &icmp_hdr, sizeof icmp_hdr);
  memcpy (packetdata + sizeof(icmp_hdr), PING_MESSAGE, sizeof PING_MESSAGE);

  // Set the TTL to something high
  int ttl = UINT8_MAX;
  setsockopt (sock, IPPROTO_IP, IP_TTL, &ttl, sizeof ttl);

  // Send the packet; if it fails due to blocking then we retry but with the flag disabled.
  if (sendto (sock, packetdata, sizeof packetdata, MSG_DONTWAIT, (struct sockaddr *) &sock_addr, sizeof sock_addr) < 0) {
    // If it failed then we should free the socket we allocated
    close (sock);
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
      errno = 0;
      return -1;
    }
    PANIC ("sendto");
  }

  return sock;
}

/* Advance a ping task to a different status (like T_DONE or T_SENT).
 * This is called whenever an event arrives for a task, or if a task timed out.
 * If a T_NOTSENT task fails, it will revert to T_NONE.
 * Returns 0 on successful advancement or -1 if it could not advance (T_NONE).
 */
int
ping_task_advance (struct ping_task *task, int epoll_fd, bool timed_out)
{
  if (task->status == T_NONE) return -1;
  if (task->status == T_DONE) return 0;

  if (task->status == T_NOTSENT) {
    // Try to send the ping.
    if (0 > (task->sock = ping_send (task->addr))) {
      task->status = T_NONE;
      return -1;
    }

    task->timeout_end = time (NULL) + PING_TIMEOUT;
    task->status = T_SENT;

    // Subscribe to events for this file descriptor
    task->epoll_obj.events = EPOLLIN;
    task->epoll_obj.data.fd = task->sock;
    task->epoll_obj.data.ptr = task;

    CHECK (0 > epoll_ctl (epoll_fd, EPOLL_CTL_ADD, task->sock, &task->epoll_obj));
  }

  if (task->status == T_SENT) {
    char recv_buf[256];

    if (timed_out) {
      // Skip checking the socket if we know it has timed out
      task->reason = P_NOREPLY;
      task->status = T_DONE;
    } else if (0 < recv (task->sock, recv_buf, sizeof recv_buf, MSG_DONTWAIT)) {
      // TODO: validate the reply packet to make sure it's not just a router being helpful
      task->reason = P_REPLIED;
      task->status = T_DONE;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
      // Error other than failure with MSG_DONTWAIT
      PANIC ("recv");
    } else if (time (NULL) >= task->timeout_end) {
      // The initial request is timed out
      task->reason = P_NOREPLY;
      task->status = T_DONE;
    }

    // Free task's resources.
    if (task->status == T_DONE) {
      CHECK (0 > epoll_ctl(epoll_fd, EPOLL_CTL_DEL, task->sock, NULL));
      CHECK (close (task->sock));
    }
  }
  return task->reason;
}

/* Initialize the ping_task object needed for the first call to ping_task_advance */
void
ping_task_init (struct ping_task *task, ipaddr addr)
{
  task->addr = addr;
  task->status = T_NOTSENT;
  task->reason = P_UNKNOWN;
}

/* Takes a finished ping task and cleans up: removes it from the list, writes to disk, and resets the state.
 */
void
ping_task_done (struct ping_task *task)
{
  ASSERT (task->status == T_DONE);

  throughput_tick (task->reason);

  // Write to memory-mapped region
  pthread_mutex_lock (&ping_lock);
  pings[task->addr] = task->reason;
  pthread_mutex_unlock (&ping_lock);

  // Remove task from its list
  list_remove (&task->elem);
  task->elem.next = NULL;
  task->elem.prev = NULL;

  ping_task_erase (task);
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
