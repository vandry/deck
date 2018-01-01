#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "global.h"
#include "cardclient.h"
#include "util.h"

int
main(int argc, char **argv)
{
	int stdio_is_tty[3];
	int ttyfd;
	char *var;
	struct sockaddr_un cardserver_socket_name;
	struct tty_settings ts;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s command [args...]\n"
			"Starts the given command in a card using the\n"
			"cardserver that exists in the environment.\n",
			argv[0]);
		return 3;
	}
	var = getenv(CARDDECK_SOCKET_VAR_NAME);
	if ((!var) || (!(*var))) {
		fprintf(stderr, "No $" CARDDECK_SOCKET_VAR_NAME ". "
			"Will exec child without doing anything instead.\n");
fallback:
		execvp(argv[1], argv+1);
		perror("exec");
		return 1;
	}
	ttyfd = stdio_connected_to_tty(&(stdio_is_tty[0]));
	if (ttyfd == -1) {
		fprintf(stderr, "This program is designed to run on a tty. "
			"Will exec child without doing anything instead.\n");
		goto fallback;
	}
	collect_tty_settings(ttyfd, &ts);

	int sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock < 0) {
		perror("socket");
		goto fallback;
	}
	memset(&cardserver_socket_name, 0, sizeof(cardserver_socket_name));
	cardserver_socket_name.sun_family = AF_UNIX;
	strncpy(cardserver_socket_name.sun_path, var, sizeof(cardserver_socket_name.sun_path)-1);
	if (connect(sock,
			(struct sockaddr*)&cardserver_socket_name,
			sizeof(cardserver_socket_name)) < 0) {
		perror("connect to cardserver");
		close(sock);
		goto fallback;
	}

	int status = cardclient(sock, &(stdio_is_tty[0]), &ts, -1, argv+1);
	exit(status);
}
