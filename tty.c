#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <termios.h>
#include <pthread.h>
#include "interactor.h"
#include "util.h"

/* This is a dumb sample implementation of the interactor.
   It assumes all input if the card 0.
   It brackets output for non-0 cards in
   "From card # {{{foobar}}}".
   That's it.
*/

struct tty_interactor {
	struct interactor base;
	int fd;
	int active_card;
	void (*input_callback)(void *data, size_t count, int card_number, void *arg);
	void *callback_arg;
	int can_restore_termios;
	struct termios termios_for_restore;
};

static void
write_sequence(int fd, const char *seq, size_t len)
{
	size_t written = 0;
	struct pollfd pollfd;
	pollfd.fd = fd;
	pollfd.events = POLLOUT;
	while (written < len) {
		if (poll(&pollfd, 1, -1) < 0) {
			sleep(1);
			continue;
		}
		size_t n = write(fd, seq+written, len-written);
		if (n <= 0) {
			sleep(1);
			continue;
		}
		written += n;
	}
}

static void
tty_interactor_claim(struct interactor *i, int card_number)
{
	struct tty_interactor *tty = (struct tty_interactor *)i;

	char buf[100];
	if (tty->active_card == card_number) {
		return;
	}
	if (card_number != 0) {
		sprintf(buf, "From card number %d {{{", card_number);
		write_sequence(tty->fd, buf, strlen(buf));
	}
	tty->active_card = card_number;
}

static void
tty_interactor_claim_none(struct interactor *i)
{
	struct tty_interactor *tty = (struct tty_interactor *)i;

	const char *seq = "}}}\n";
	if (tty->active_card == 0) return;
	write_sequence(tty->fd, seq, strlen(seq));
	tty->active_card = 0;
}

static ssize_t
tty_interactor_write(struct interactor *i, const void *buf, size_t count)
{
	struct tty_interactor *tty = (struct tty_interactor *)i;
	return write(tty->fd, buf, count);
}

static void *
get_input(void *arg)
{
	struct pollfd pollfd;
	struct tty_interactor *tty = (struct tty_interactor *)arg;
	char buf[4096];

	pollfd.fd = tty->fd;
	pollfd.events = POLLIN;

	for (;;) {
		int n = poll(&pollfd, 1, -1);
		if (n <= 0) {
			if (n == 0) continue;
			if (errno == EINTR) continue;
			if (errno == EAGAIN) continue;
			sleep(1);
			continue;
		}
		if (pollfd.revents & POLLHUP) {
			break;
		}
		size_t nread = read(tty->fd, &(buf[0]), sizeof(buf));
		if (nread <= 0) {
			if ((nread < 0) && (errno == EAGAIN)) continue;
			if ((nread < 0) && (errno == EINTR)) continue;
			break;
		}

		/* blindly assume that all input is for card 0 for now. */
		tty->input_callback(&(buf[0]), nread, 0, tty->callback_arg);
	}
	tty->input_callback(NULL, 0, 0, tty->callback_arg);
	return NULL;
}

static void
tty_interactor_destroy(struct interactor *i)
{
	struct tty_interactor *tty = (struct tty_interactor *)i;
	if (tty->can_restore_termios) {
		tcsetattr(tty->fd, TCSANOW, &(tty->termios_for_restore));
	}
}

static void
tty_set_input_callback(
	struct interactor *i,
	void (*input_callback)(void *data, size_t count, int card_number, void *arg),
	void *callback_arg
)
{
	struct tty_interactor *tty = (struct tty_interactor *)i;
	tty->input_callback = input_callback;
	tty->callback_arg = callback_arg;
}

static int
tty_interactor_check_ready(struct interactor *i, struct pollfd *pfd)
{
	struct tty_interactor *tty = (struct tty_interactor *)i;

	pfd->fd = tty->fd;
	pfd->events = POLLOUT;
	return 1;
}

const struct interactor_interface tty_interactor_interface = {
	.set_input_callback = tty_set_input_callback,
	.destroy = tty_interactor_destroy,
	.write = tty_interactor_write,
	.claim = tty_interactor_claim,
	.claim_none = tty_interactor_claim_none,
	.check_ready_for_output = tty_interactor_check_ready,
};

struct interactor *
new_interactor(int fd)
{
	pthread_t thread_id;
	pthread_attr_t thread_attr;
	struct termios tio;

	struct tty_interactor *tty = malloc(sizeof(struct tty_interactor));
	if (!tty) return NULL;
	tty->base.intf = &tty_interactor_interface;
	tty->fd = fd;
	tty->active_card = 0;
	tty->can_restore_termios = 0;
	setnonblock(fd);

	if (tcgetattr(fd, &tio) == 0) {
		memcpy(&(tty->termios_for_restore), &(tio), sizeof(tio));
		tty->can_restore_termios = 1;
		cfmakeraw(&tio);
		tio.c_lflag &= ~ECHO;
		tcsetattr(fd, TCSANOW, &tio);
	}

	pthread_attr_init(&thread_attr);
	pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
	pthread_create(&thread_id, &thread_attr, get_input, tty);

	return (struct interactor *)tty;
}
