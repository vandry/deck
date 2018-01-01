#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include "cardclient.h"
#include "cardserver.h"
#include "renderer.h"
#include "util.h"

int
main(int argc, char **argv)
{
	int stdio_is_tty[3];
	int ttyfd;
	struct tty_settings ts;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s command [args...]\n"
			"Starts the given command under a subordinate pty and\n"
			"with a cardserver socket so that commands in the\n"
			"current session can move themselves to sub-terminals\n"
			"or sub-cards of the main one. The I/O on this\n"
			"command's original tty becomes a multiplexed stream\n"
			"of the IO on the main card and all its sub-cards.\n",
			argv[0]);
		return 3;
	}
	ttyfd = stdio_connected_to_tty(&(stdio_is_tty[0]));
	if (ttyfd == -1) {
		fprintf(stderr, "This program is designed to run on a tty. "
			"Will exec child without doing anything instead.\n");
fallback:
		execvp(argv[1], argv+1);
		perror("exec");
		return 1;
	}
	collect_tty_settings(ttyfd, &ts);

	int sv[2];
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, &(sv[0])) < 0) {
		perror("socketpair");
		goto fallback;
	}

	struct renderer *renderer = new_renderer(ttyfd);
	if (!renderer) {
		perror("no renderer");
fallback2:
		close(sv[0]);
		close(sv[1]);
		goto fallback;
	}

	struct cardserver *srv = cardserver(renderer, sv[0]);
	if (!srv) {
		goto fallback2;
	}

	/* The main thread becomes card #0 */
	int status = cardclient(sv[1], &(stdio_is_tty[0]),
		&ts, sv[0], argv+1);

	cardserver_quit(srv);
	exit(status);
}
