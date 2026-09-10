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
	static const char request[] = "dual-stack-request";
	static const char response[] = "dual-stack-response";
	const struct in6_addr expected_peer = {
		.s6_addr = { 0, 0, 0, 0, 0, 0, 0, 0,
			     0, 0, 0xff, 0xff, 127, 0, 0, 1 },
	};
	struct sockaddr_in6 wildcard = {
		.sin6_family = AF_INET6,
		.sin6_addr = IN6ADDR_ANY_INIT,
	};
	struct sockaddr_in client_addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	struct sockaddr_in client_local = { 0 };
	struct sockaddr_in6 peer = { 0 };
	socklen_t wildcard_len = sizeof(wildcard);
	socklen_t client_local_len = sizeof(client_local);
	socklen_t peer_len = sizeof(peer);
	char buffer[sizeof(response) > sizeof(request) ? sizeof(response) : sizeof(request)];
	int listener = -1;
	int client = -1;
	int conflicting = -1;
	int accepted = -1;
	int v6only = 0;

	signal(SIGALRM, timeout_handler);
	alarm(10);

	listener = socket(AF_INET6, SOCK_STREAM, 0);
	if (listener < 0 ||
	    setsockopt(listener, IPPROTO_IPV6, IPV6_V6ONLY, &v6only,
		       sizeof(v6only)) < 0 ||
	    bind(listener, (struct sockaddr *)&wildcard, sizeof(wildcard)) < 0 ||
	    listen(listener, 1) < 0 ||
	    getsockname(listener, (struct sockaddr *)&wildcard, &wildcard_len) < 0 ||
	    wildcard.sin6_port == 0)
		goto fail;

	conflicting = socket(AF_INET, SOCK_STREAM, 0);
	if (conflicting < 0)
		goto fail;
	client_addr.sin_port = wildcard.sin6_port;
	errno = 0;
	if (bind(conflicting, (struct sockaddr *)&client_addr, sizeof(client_addr)) != -1 ||
	    errno != EADDRINUSE)
		goto fail;
	close(conflicting);
	conflicting = -1;

	client = socket(AF_INET, SOCK_STREAM, 0);
	if (client < 0)
		goto fail;
	if (connect(client, (struct sockaddr *)&client_addr, sizeof(client_addr)) < 0)
		goto fail;
	if (getsockname(client, (struct sockaddr *)&client_local, &client_local_len) < 0 ||
	    client_local.sin_port == wildcard.sin6_port)
		goto fail;

	accepted = accept(listener, (struct sockaddr *)&peer, &peer_len);
	if (accepted < 0 || peer.sin6_family != AF_INET6 ||
	    memcmp(&peer.sin6_addr, &expected_peer, sizeof(expected_peer)) != 0)
		goto fail;
	if (send(client, request, sizeof(request), 0) != sizeof(request))
		goto fail;
	if (recv(accepted, buffer, sizeof(request), 0) != sizeof(request) ||
	    memcmp(buffer, request, sizeof(request)) != 0)
		goto fail;
	if (send(accepted, response, sizeof(response), 0) != sizeof(response) ||
	    recv(client, buffer, sizeof(response), 0) != sizeof(response) ||
	    memcmp(buffer, response, sizeof(response)) != 0)
		goto fail;

	close(accepted);
	close(client);
	close(listener);
	puts("ASTERINAS_IPV6_DUAL_STACK_TCP_OK peer=::ffff:127.0.0.1 client-port-reserved=1");
	return 0;

fail:
	fprintf(stderr, "ASTERINAS_IPV6_DUAL_STACK_TCP_FAIL errno=%d\n", errno);
	if (accepted >= 0)
		close(accepted);
	if (conflicting >= 0)
		close(conflicting);
	if (client >= 0)
		close(client);
	if (listener >= 0)
		close(listener);
	return 1;
}
