// SPDX-License-Identifier: MPL-2.0

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void timeout_handler(int signal_number)
{
	(void)signal_number;
	_exit(124);
}

int main(void)
{
	static const char request[] = "dual-stack-udp-request";
	static const char response[] = "dual-stack-udp-response";
	const struct in6_addr expected_peer = {
		.s6_addr = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 127, 0,
			     0, 1 },
	};
	struct sockaddr_in6 wildcard = {
		.sin6_family = AF_INET6,
		.sin6_addr = IN6ADDR_ANY_INIT,
	};
	struct sockaddr_in destination = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	struct sockaddr_in sender_local = { 0 };
	struct sockaddr_in sender_peer = { 0 };
	struct sockaddr_in6 peer = { 0 };
	socklen_t wildcard_len = sizeof(wildcard);
	socklen_t sender_local_len = sizeof(sender_local);
	socklen_t sender_peer_len = sizeof(sender_peer);
	socklen_t peer_len = sizeof(peer);
	char buffer[sizeof(response) > sizeof(request) ? sizeof(response) :
							 sizeof(request)];
	int listener = -1;
	int sender = -1;
	int v6only = 0;
	ssize_t sent;
	ssize_t received;
	ssize_t response_sent;
	ssize_t response_received;

	signal(SIGALRM, timeout_handler);
	alarm(10);

	listener = socket(AF_INET6, SOCK_DGRAM, 0);
	fprintf(stderr,
		"ASTERINAS_IPV6_DUAL_STACK_UDP_STAGE socket=%d errno=%d\n",
		listener, errno);
	if (listener < 0 ||
	    setsockopt(listener, IPPROTO_IPV6, IPV6_V6ONLY, &v6only,
		       sizeof(v6only)) < 0 ||
	    bind(listener, (struct sockaddr *)&wildcard, sizeof(wildcard)) <
		    0 ||
	    getsockname(listener, (struct sockaddr *)&wildcard, &wildcard_len) <
		    0 ||
	    wildcard.sin6_port == 0)
		goto fail;
	fprintf(stderr, "ASTERINAS_IPV6_DUAL_STACK_UDP_STAGE bound port=%u\n",
		ntohs(wildcard.sin6_port));

	sender = socket(AF_INET, SOCK_DGRAM, 0);
	fprintf(stderr,
		"ASTERINAS_IPV6_DUAL_STACK_UDP_STAGE sender=%d errno=%d\n",
		sender, errno);
	if (sender < 0)
		goto fail;
	destination.sin_port = wildcard.sin6_port;
	sent = sendto(sender, request, sizeof(request), 0,
		      (struct sockaddr *)&destination, sizeof(destination));
	fprintf(stderr,
		"ASTERINAS_IPV6_DUAL_STACK_UDP_STAGE sent=%zd errno=%d\n", sent,
		errno);
	if (sent != sizeof(request) ||
	    getsockname(sender, (struct sockaddr *)&sender_local,
			&sender_local_len) < 0)
		goto fail;
	fprintf(stderr,
		"ASTERINAS_IPV6_DUAL_STACK_UDP_STAGE local-port=%u listener-port=%u\n",
		ntohs(sender_local.sin_port), ntohs(wildcard.sin6_port));
	if (sender_local.sin_port == wildcard.sin6_port)
		goto fail;
	received = recvfrom(listener, buffer, sizeof(buffer), 0,
			    (struct sockaddr *)&peer, &peer_len);
	fprintf(stderr,
		"ASTERINAS_IPV6_DUAL_STACK_UDP_STAGE received=%zd errno=%d\n",
		received, errno);
	if (received != sizeof(request) || peer.sin6_family != AF_INET6 ||
	    memcmp(&peer.sin6_addr, &expected_peer, sizeof(expected_peer)) !=
		    0 ||
	    memcmp(buffer, request, sizeof(request)) != 0)
		goto fail;
	fprintf(stderr, "ASTERINAS_IPV6_DUAL_STACK_UDP_STAGE received\n");

	response_sent = sendto(listener, response, sizeof(response), 0,
			       (struct sockaddr *)&peer, peer_len);
	fprintf(stderr,
		"ASTERINAS_IPV6_DUAL_STACK_UDP_STAGE response-sent=%zd errno=%d\n",
		response_sent, errno);
	if (response_sent != sizeof(response))
		goto fail;
	response_received = recvfrom(sender, buffer, sizeof(buffer), 0,
				     (struct sockaddr *)&sender_peer,
				     &sender_peer_len);
	fprintf(stderr,
		"ASTERINAS_IPV6_DUAL_STACK_UDP_STAGE response-received=%zd errno=%d family=%d\n",
		response_received, errno, sender_peer.sin_family);
	if (response_received != sizeof(response) ||
	    sender_peer.sin_family != AF_INET ||
	    memcmp(buffer, response, sizeof(response)) != 0)
		goto fail;

	close(sender);
	close(listener);
	puts("ASTERINAS_IPV6_DUAL_STACK_UDP_OK peer=::ffff:127.0.0.1 client-port-reserved=1");
	return 0;

fail:
	fprintf(stderr, "ASTERINAS_IPV6_DUAL_STACK_UDP_FAIL errno=%d\n", errno);
	if (sender >= 0)
		close(sender);
	if (listener >= 0)
		close(listener);
	return 1;
}
