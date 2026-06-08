#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>

#include "ping.h"
#include "logging.h"
#include "worker.h"

// synchronize access to the mapped file
pthread_mutex_t ping_lock;
uint8_t *pings;

/* Send a ping. This function blocks.
 * Returns 0 on success and -1 if send failed.
 */
int
ping_send (int sock, ipaddr addr, int seq)
{
  // Source: https://sturmflut.github.io/linux/ubuntu/2015/01/17/unprivileged-icmp-sockets-on-linux/
  // This helped me with the socket creation, though I've used my own before.
  struct sockaddr_in sock_addr = { 0 };
  struct icmphdr icmp_hdr = { 0 };

  // Initialize the destination address
  sock_addr.sin_family = AF_INET;
  sock_addr.sin_addr.s_addr = htonl (addr);

  // Initialize the ICMP header
  icmp_hdr.type = ICMP_ECHO;
  icmp_hdr.un.echo.sequence = seq;

  // sendto(2): "Packets are just silently dropped when a device queue overflows."
  while (0 > sendto (sock, &icmp_hdr, sizeof icmp_hdr, 0, (struct sockaddr *) &sock_addr, sizeof sock_addr))
    if (errno != EINTR)
      return -1;
  return 0;
}

/* Returns -1 on recvfrom error, 0 otherwise.
 * This function blocks.
 */
int
ping_recv (int sock)
{
  char recv_buf[1024];
  struct sockaddr_in sender_saddr;
  in_addr_t sender_addr;
  socklen_t size = sizeof sender_saddr;
  struct icmphdr *hdr;
  int seq;

  while (0 > recvfrom (sock, recv_buf, sizeof recv_buf, 0, (struct sockaddr *) &sender_saddr, &size))
    if (errno != EINTR)
      return -1;

  hdr = (void *)recv_buf;
  seq = hdr->un.echo.sequence;
  ASSERT (hdr->type == ICMP_ECHOREPLY);

  // Check source IP of received packet.
  sender_addr = ntohl (sender_saddr.sin_addr.s_addr);

  // Check sequence number of ICMP packet for bounds
  // If OOB then this packet is invalid / ignored
  if (!(MIN_SEQ <= seq && seq <= MAX_SEQ)) {
    debug ("Sequence (%d) is OOB", seq);
    return 0;
  }

  // Write the sequence number at the sender's address
  pthread_mutex_lock (&ping_lock);
  pings[sender_addr] |= SEQ_TO_BIT (seq);
  pthread_mutex_unlock (&ping_lock);
  return 0;
}

int
count_replies (int bits)
{
  int count = 0;
  for (int i = 0; i < NUM_SENDS; ++i)
    if (bits & SEQ_TO_BIT (i))
      count++;
  return count;
}

/* Create a socket and initialize its options. Returns -1 on socket creation error and 0 for success. */
int
socket_create (void)
{
  int sock, ttl;

  sock = socket (AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
  if (0 > sock)
    return -1;

  // Increase TTL
  ttl = UINT8_MAX;
  if (0 > setsockopt (sock, IPPROTO_IP, IP_TTL, &ttl, sizeof ttl))
    return -1;

  // https://stackoverflow.com/questions/4181784/how-to-set-socket-timeout-in-c-when-making-multiple-connections
  struct timeval timeout;
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;

  if (0 > setsockopt (sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) ||
    (0 > setsockopt (sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout)))
    return -1;

  return sock;
}

/* Gives a string from an IPv4 address. Cannot be used recursively or ephemerally. Thread-safe. */
char *
ip_ntoa(ipaddr addr)
{
  struct in_addr in_addr = {
    .s_addr = htonl (addr),
  };
  return inet_ntoa (in_addr);
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
