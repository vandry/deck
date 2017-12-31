#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include "cardclient.h"
#include "cardserver.h"
#include "interactor.h"
#include "util.h"

int
main(int argc, char **argv)
{
	int stdio_is_tty[3];
	int ttyfd;
	char cardserver_socket_dir_template[30];
	char env_var[50];
	char *cardserver_socket_dir;
	int cardserver_master;
	struct sockaddr_un cardserver_socket_name;
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

	/* Our child process will be the first card of the cardserver. */
	/* Set up the short-circuited socket through which that will happen. */
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, &(sv[0])) < 0) {
		perror("socketpair");
		goto fallback;
	}

	sprintf(cardserver_socket_dir_template, "/tmp/cardserver.XXXXXX");
	cardserver_socket_dir = mkdtemp(cardserver_socket_dir_template);
	if (!cardserver_socket_dir) {
		perror("mkdtemp");
		goto fallback;
	}
	cardserver_master = socket(AF_UNIX, SOCK_STREAM, 0);
	if (cardserver_master < 0) {
		perror("socket");
fallback2:
		rmdir(cardserver_socket_dir);
		goto fallback;
	}
	memset(&cardserver_socket_name, 0, sizeof(cardserver_socket_name));
	cardserver_socket_name.sun_family = AF_UNIX;
	sprintf(cardserver_socket_name.sun_path, "%s/sock", cardserver_socket_dir);
	if (bind(cardserver_master,
			(struct sockaddr*)&cardserver_socket_name,
			sizeof(cardserver_socket_name)) < 0) {
		perror("bind cardserver master socket");
fallback3:
		close(cardserver_master);
		unlink(cardserver_socket_name.sun_path);
		goto fallback2;
	}

	struct interactor *interactor = new_interactor(ttyfd);
	if (!interactor) {
		perror("no interactor");
		goto fallback3;
	}
	struct cardserver *srv = cardserver(interactor, cardserver_master, sv[0]);

	sprintf(env_var, "CARDSERVER_SOCKET=%s", cardserver_socket_name.sun_path);
	putenv(env_var);

	/* The main thread becomes card #0 */
	int status = cardclient(sv[1], &(stdio_is_tty[0]),
		&ts, cardserver_master, argv+1);

	cardserver_quit(srv);
	unlink(cardserver_socket_name.sun_path);
	rmdir(cardserver_socket_dir);
	exit(status);
}
