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

int
ping_send (ipaddr addr)
{
  // Source: https://sturmflut.github.io/linux/ubuntu/2015/01/17/unprivileged-icmp-sockets-on-linux/
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

  // Double send buffer
  int send_size;
  socklen_t len = sizeof (send_size);
  getsockopt (sock, SOL_SOCKET, SO_SNDBUF, &send_size, &len);
  send_size *= 2;
  setsockopt (sock, SOL_SOCKET, SO_SNDBUF, &send_size, sizeof send_size);

  int send_flag = MSG_DONTWAIT;

  // Send the packet; if it fails due to blocking then we retry but with the flag disabled.
  while (sendto (sock, packetdata, sizeof packetdata, send_flag, (struct sockaddr*) &sock_addr, sizeof sock_addr) < 0) {
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
      send_flag = 0;
      continue;
    }
    close (sock);
    PANIC ("sendto");
  }

  return sock;
}

enum PingReason
ping_task (struct ping_task *task, int epoll_fd)
{
  if (task->status == T_NONE)
    return P_UNKNOWN;
  if (task->status == T_DONE)
    return task->reason;

  if (task->status == T_NOTSENT) {
    // Send the ping.
    task->sock = ping_send (task->addr);
    task->timeout_end = time (NULL) + PING_TIMEOUT;
    task->status = T_SENT;

    //debug ("ping_send: %s", ip_htos (task->addr));
  }

  if (task->status == T_SENT) {
    char recv_buf[256];
    if (0 < recv (task->sock, recv_buf, sizeof recv_buf, MSG_DONTWAIT)) {
      // TODO: validate the reply packet to make sure it's not just a router being helpful
      CHECK (0 > epoll_ctl(epoll_fd, EPOLL_CTL_DEL, task->sock, NULL));
      CHECK (close (task->sock));
      task->reason = P_REPLIED;
      task->status = T_DONE;
    } else {
      // We encountered an error (likely telling us the buffer's empty)
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        CHECK (0 > epoll_ctl(epoll_fd, EPOLL_CTL_DEL, task->sock, NULL));
        CHECK (close (task->sock));
        PANIC ("recv");
      }

      // The initial request is timed out
      if (time (NULL) >= task->timeout_end) {
        CHECK (0 > epoll_ctl(epoll_fd, EPOLL_CTL_DEL, task->sock, NULL));
        CHECK (close (task->sock));
        task->reason = P_NOREPLY;
        task->status = T_DONE;
      }
    }
  }
  return task->reason;
}

void
ping_task_start_new (struct ping_task *task, ipaddr cur, int epoll_fd)
{
  // Initialize
  *task = (struct ping_task) {
    .addr = cur,
    .status = T_NOTSENT,
    .reason = P_UNKNOWN,
    .sock = -1,
    .timeout_end = 0,
    .epoll_obj = {0},
  };
  // Start the ping task
  ping_task (task, epoll_fd);

  // Subscribe to events for this file descriptor
  task->epoll_obj.events = EPOLLIN;
  task->epoll_obj.data.fd = task->sock;
  task->epoll_obj.data.ptr = task;

  CHECK (0 > epoll_ctl (epoll_fd, EPOLL_CTL_ADD, task->sock, &task->epoll_obj));
}

/* Return 1 if task was replaced, 0 if task had no change */
int
ping_task_look_renew (
  struct ping_task *task,
  struct list *tasks_waiting,
  ipaddrl *cur, ipaddrl end,
  int epoll_fd)
{
  // Check up on the ping task
  struct timespec start_time, end_time;
  clock_gettime (CLOCK_MONOTONIC_RAW, &start_time);
  ping_task (task, epoll_fd);
  clock_gettime (CLOCK_MONOTONIC_RAW, &end_time);
  uint64_t delta_ms = timespec_diff_ms (start_time, end_time);
  if (delta_ms > 30)
    debug ("%lu elapsed in ping()", delta_ms);

  if (task->status == T_DONE) {
    // Print the IP address and reply type
    //debug ("%s %s", ip_htos (task->addr), (task->reason == P_REPLIED) ? "online" : "unreachable");
    throughput_tick (task->reason);

    // Write to memory-mapped region
    pthread_mutex_lock (&ping_lock);
    pings[task->addr] = task->reason;
    pthread_mutex_unlock (&ping_lock);

    list_remove (&task->elem);

    // Fill this slot
    if (*cur <= UINT32_MAX) {
      ping_task_start_new (task, *cur, epoll_fd);
      list_push_back (tasks_waiting, &task->elem);
      *cur = pings_next_unknown(*cur + 1, end);
    }

    return 1;
  }
  return 0;
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
ping_task_init (struct ping_task *t)
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
