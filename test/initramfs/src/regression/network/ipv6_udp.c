// SPDX-License-Identifier: MPL-2.0

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

int main(void) {
  int fd = socket(AF_INET6, SOCK_DGRAM, 0);
  assert(fd >= 0);

  int v6only = -1;
  socklen_t optlen = sizeof(v6only);
  assert(getsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, &optlen) == 0);
  assert(v6only == 1);

  v6only = 0;
  assert(setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) == 0);
  v6only = -1;
  optlen = sizeof(v6only);
  assert(getsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, &optlen) == 0);
  assert(v6only == 0);

  struct sockaddr_in6 addr = {
      .sin6_family = AF_INET6,
      .sin6_addr = IN6ADDR_LOOPBACK_INIT,
      .sin6_port = 0,
  };
  assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);

  socklen_t addrlen = sizeof(addr);
  assert(getsockname(fd, (struct sockaddr *)&addr, &addrlen) == 0);
  assert(addr.sin6_family == AF_INET6);
  assert(addr.sin6_port != 0);

  int sender = socket(AF_INET6, SOCK_DGRAM, 0);
  assert(sender >= 0);

  const struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
         0);

  const char payload[] = "asterinas-ipv6-udp";
  assert(sendto(sender, payload, sizeof(payload), 0, (struct sockaddr *)&addr,
                sizeof(addr)) == (ssize_t)sizeof(payload));

  char received[sizeof(payload)] = {0};
  assert(recvfrom(fd, received, sizeof(received), 0, NULL, NULL) ==
         (ssize_t)sizeof(payload));
  assert(memcmp(payload, received, sizeof(payload)) == 0);

  close(sender);
  close(fd);
  puts("ipv6_udp: PASS");
  return 0;
}
