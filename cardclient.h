#ifndef _DECK_CARDCLIENT_H
#define _DECK_CARDCLIENT_H

/* A cardclient:
     - connects to the running cardserver
     - runs a subordinate command in a new pty as a child process
     - sends all io on the master side of that pty to the cardserver.
   The effect is that the subprocess will get its own card for all its io.
*/

#include <termios.h>
#include <sys/ioctl.h>

struct tty_settings {
        struct termios attrs;
        struct winsize win;
	struct termios *attrsp;
	struct winsize *winp;
};

/* Collects TTY settings from some terminal, presumably the one on
   which the cardclient was started. These settings are used to
   initialize the settings of the new pty we will create. */
void collect_tty_settings(int ttyfd, struct tty_settings *);

/* Returns the child's exit status */
int cardclient(
	/* Must be already connected. */
	int sock_to_cardserver,
	/* Array of 3 ints. Indicates which stdio fds already have
	   a tty connected. Those and only those will, in the child
	   process, be replaced with the new pty we allocate. */
	int *stdio_is_tty,
	/* TTY settings previously collected with which we should
	   initialize the new pty. */
	struct tty_settings *,
	/* In case you need it (deck.c does). If not used, set to -1. */
	int extra_fd_to_close_in_child,
	/* The child process to run. */
	char **argv
);

#endif /* _DECK_CARDCLIENT_H */
