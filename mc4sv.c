/*-
 * SPDX short identifier: BSD-2-Clause
 *
 * Copyright (c) 2022 KusaReMKN.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>

#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <err.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUFFERSIZE 2048

static sigjmp_buf env;

static void alarm_handler(int);
static void get_inaddr(char *, struct in_addr *);
static void usage(void);

int
main(int argc, char *argv[])
{
	int ch, error, fd, quiet;
	time_t timeout;
	ssize_t received;
	size_t packets, sumsize;
	socklen_t slen;
	char *cause, *mcast = "224.0.0.1", *service = "discard";
	char buffer[BUFFERSIZE], ifname[IFNAMSIZ] = "";
	struct addrinfo *ai, *res, hints;
	struct ip_mreq imr;
	struct sigaction sa;
	struct itimerval it;
	struct sockaddr_in sin;

	quiet = 0;
	timeout = 0;
	while ((ch = getopt(argc, argv, "i:qt:")) != -1)
		switch (ch) {
		case 'i':
			if (strlen(optarg) >= sizeof(ifname))
				errx(1, "%s: interface name too long", optarg);
			strncpy(ifname, optarg, sizeof(ifname));
			break;
		case 'q':
			quiet = 1;
			break;
		case 't':
			timeout = atoi(optarg);
			if (timeout > 3600)
				errx(1, "%s: invalid timeout (>3600)", optarg);
			break;
		case '?':
		default:
			usage();
			/* NOTREACHED */
		}
	argc -= optind;
	argv += optind;

	switch (argc) {
	case 2:
		service = argv[1];
		/* FALLTHROUGH */
	case 1:
		mcast = argv[0];
		/* FALLTHROUGH */
	case 0:
		break;
	default:
		usage();
		/* NOTREACHED */
	}

	if (inet_aton(mcast, &imr.imr_multiaddr) == 0)
		errx(1, "%s: invalid multicast group", mcast);
	if (ifname[0] != '\0')
		get_inaddr(ifname, &imr.imr_interface);
	else
		imr.imr_interface.s_addr = htonl(INADDR_ANY);

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_PASSIVE;
	error = getaddrinfo(NULL, service, &hints, &res);
	if (error != 0)
		errx(1, "%s", gai_strerror(error));
	for (ai = res; ai != NULL; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd == -1) {
			cause = "socket";
			continue;
		}
		if (bind(fd, ai->ai_addr, ai->ai_addrlen) == -1) {
			cause = "bind";
			close(fd);
			continue;
		}
		if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &imr,
					sizeof(imr)) == -1) {
			cause = "IP_ADD_MEMBERSHIP";
			close(fd);
			continue;
		}
		break;	/* SUCCESS */
	}
	if (ai == NULL)
		err(1, "%s", cause);
	freeaddrinfo(res);

	sa.sa_handler = alarm_handler;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGALRM, &sa, NULL) == -1)
		err(1, "sigaction: SIGALRM");
	if (sigaction(SIGINT, &sa, NULL) == -1)
		err(1, "sigaction: SIGINT");

	if (sigsetjmp(env, 1) != 0)
		goto quit;

	memset(&it, 0, sizeof(it));
	it.it_value.tv_sec = timeout;
	if (setitimer(ITIMER_REAL, &it, NULL) == -1)
		err(1, "setitimer");

	packets = 0;
	sumsize = 0;
	for (;;) {
		slen = sizeof(sin);
		received = recvfrom(fd, buffer, sizeof(buffer), 0,
				(struct sockaddr *)&sin, &slen);
		packets++;
		sumsize += received;
		if (!quiet)
			printf("received from %s:%u (%zu)\n",
					inet_ntoa(sin.sin_addr), sin.sin_port,
					(size_t)received);
	}
quit:
	printf("\n%zu packets (%zu byte) received.\n", packets, sumsize);

	close(fd);

	return 0;
}

static void
alarm_handler(int sig)
{
	siglongjmp(env, 1);
}

static void
get_inaddr(char *ifname, struct in_addr *addr)
{
	struct ifaddrs *ifa, *res;

	if (getifaddrs(&res) == -1)
		err(1, "getifaddrs");
	for (ifa = res; ifa != NULL; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr == NULL
				|| ifa->ifa_addr->sa_family != AF_INET)
			continue;
		if (strcmp(ifname, ifa->ifa_name) == 0)
			break;
	}
	if (ifa == NULL)
		errx(1, "%s: interface does not exist or is invalid", ifname);
	*addr = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
	freeifaddrs(res);
}

static void
usage(void)
{
	(void)fprintf(stderr,
			"mc4sv [-i interface] [-q] [-t timeout]"
			" [mcast-group [service]]\n");
	exit(1);
}
