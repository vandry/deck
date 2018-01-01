#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <string.h>
#include "cardmux.h"
#include "stub.h"
#include "util.h"
#include "interactor.h"

/* Give up the tty no matter what this amount of time after
   writing anything to it. This will cause us to emit the
   escape sequence for telling the other end our card is no
   longer active. */
const long always_give_up_anyway_nsec = 500*1000*1000;  /* 500ms */

/* If we still have stuff to write and we haven't been able
   to get it out in this amount of time, and somebody else
   wants the tty, give it up. */
const long short_give_up_nsec = 10*1000*1000;  /* 10ms */

/* If we have written this many bytes, do offer to give up
   the tty even if we have more to write, if we've already
   written this much. */
const int maybe_give_up_if_written_bytes = 50;

struct cardinput {
	struct cardinput *next;
	void *data;
	size_t size;
};

/* Return the number of milliseconds of (t1+t1_offset)-(t0+t0_offset). */
static int
msec_until(struct timespec *t0, long t0_offset, struct timespec *t1, long t1_offset)
{
	int t1ms = (t1->tv_nsec + t1_offset) / 1000000;
	int t1s = t1->tv_sec;
	while (t1ms >= 1000) {
		t1ms -= 1000;
		t1s++;
	}

	int t0ms = (t0->tv_nsec + t0_offset) / 1000000;
	int t0s = t0->tv_sec;
	while (t0ms >= 1000) {
		t0ms -= 1000;
		t0s++;
	}

	return (t1s-t0s) * 1000 + (t1ms-t0ms);
}

static void *
copy_to_client(void *arg)
{
	struct cardclient *c = (struct cardclient *)arg;
	struct cardinput *i;
	struct pollfd pollfd;

	pollfd.fd = c->sock;
	pollfd.events = POLLOUT;

	pthread_mutex_lock(&(c->input_lock));
	while (!((c->input_stop) || (c->input_broken))) {
		i = c->input_head;
		if (!i) {
			pthread_cond_wait(&(c->input_cv), &(c->input_lock));
			continue;
		}
		c->input_head = i->next;
		if (c->input_tail == i) {
			c->input_tail = NULL;
		}

		if (i->data == NULL) {
			c->input_broken = 1;
			free(i);
			break;
		}

		while (i->size > 0) {
			int n = poll(&pollfd, 1, -1);
			if (n <= 0) {
				if (n == 0) continue;
				if (errno == EAGAIN) continue;
				if (errno == EINTR) continue;
				sleep(1);
				continue;
			}
			ssize_t nwritten = write(c->sock, i->data, i->size);
			if (nwritten <= 0) {
				c->input_broken = 1;
				break;
			}
			i->data += nwritten;
			i->size -= nwritten;
		}
		free(i);
	}
	while (!(c->input_stop)) {
		/* Quit because input_broken? */
		/* Do nothing until the copy_from_client thread quits */
		pthread_cond_wait(&(c->input_cv), &(c->input_lock));
	}
	while (c->input_head) {
		i = c->input_head;
		c->input_head = i->next;
		free(i);
	}
	pthread_mutex_unlock(&(c->input_lock));
	free(c);
	return NULL;
}

static void *
copy_from_client(void *arg)
{
	struct cardclient *c = (struct cardclient *)arg;
	int i_own_the_tty = 0;
	int somebody_else_may_want_the_tty = 0;
	struct timespec time_last_written_anything;
	struct timespec now;
	
	struct pollfd pollfd[3];
	int nfds, n;
	int client_running = 1;
	int tty_running = 1;
	char buf[4096];
	char scratch[10];
	size_t buf_fill = 0;
	size_t written_since_owning_tty;
	int try_writing;

	pollfd[0].fd = c->notify_pipe_read;
	pollfd[0].events = POLLIN;

	setnonblock(c->sock);
	setnonblock(c->notify_pipe_read);
	while (tty_running) {
		if ((!i_own_the_tty) && (buf_fill > 0)) {
			/* We need the tty before we can do anything else. */
			claim_tty(c->srv, c);
			i_own_the_tty = 1;
			written_since_owning_tty = 0;
			clock_gettime(CLOCK_MONOTONIC, &time_last_written_anything);
		}
		if ((!client_running) && (buf_fill == 0)) {
			break;
		}

		int timeout = -1;
		if (i_own_the_tty) {
			long give_up_anyway_nsec = always_give_up_anyway_nsec;
			/* Should we give it up? */
			if (buf_fill) {
				/* if we have a write backlog, we really
				   don't want to give up the tty. */
				if (
					(written_since_owning_tty > maybe_give_up_if_written_bytes) &&
					somebody_else_may_want_the_tty
				) {
					/* on the other hand, we've had our chance.
					   Definitely give up in this case. */
					give_up_tty(c->srv);
					i_own_the_tty = 0;
					continue;
				}
				if (somebody_else_may_want_the_tty) {
					/* Sure, but we need it too. */
					/* Don't keep it too long though. */
					give_up_anyway_nsec = short_give_up_nsec;
				}
			}
			if ((written_since_owning_tty > 0) || (buf_fill == 0)) {
				clock_gettime(CLOCK_MONOTONIC, &now);
				timeout = msec_until(&now, 0, &time_last_written_anything, give_up_anyway_nsec);
				if (timeout <= 0) {
					give_up_tty(c->srv);
					i_own_the_tty = 0;
					continue;
				}
			} /* else we keep the tty indefinitely in order that we can make 
			     any progress at all. */
		}

		/* entry 0 is always the notify pipe */
		nfds = 1;
		try_writing = 0;
		if (buf_fill && i_own_the_tty) {
			/* (Note that if buf_fill > 0, i_own_the_tty is guaranteed already) */
			int using_poll = c->srv->interactor->intf->check_ready_for_output(
				c->srv->interactor, &(pollfd[nfds])
			);
			if (using_poll) {
				nfds++;
			} else {
				/* The interactor told us it was ready without needing to poll. */
				timeout = 0;
				try_writing = 1;
			}
		}
		if (buf_fill < sizeof(buf)) {
			pollfd[nfds].fd = c->sock;
			pollfd[nfds++].events = POLLIN;
		}
		n = poll(&(pollfd[0]), nfds, timeout);
		if (n < 0) {
			if (errno == EAGAIN) continue;
			if (errno == EINTR) continue;
			perror("poll");
			sleep(1);
			continue;
		}

		if (pollfd[0].revents) {
			if (i_own_the_tty) {
				somebody_else_may_want_the_tty = 1;
			}
			/* soak it up but throw it away. */
			read(pollfd[0].fd, &(scratch[0]), sizeof(scratch));
		}
		for (n = 1; n < nfds; n++) {
			if (!pollfd[n].revents) continue;
			if (pollfd[n].fd == c->sock) {
				if (pollfd[n].revents & POLLHUP) {
					client_running = 0;
					continue;
				}
				size_t nread = read(c->sock, &(buf[buf_fill]), sizeof(buf)-buf_fill);
				if (nread < 0) {
					perror("read from cardclient");
					client_running = 0;
					continue;
				}
				if (nread == 0) {
					client_running = 0;
					continue;
				}
				buf_fill += nread;
			} else {
				/* must be the interactor */
				if (pollfd[n].revents & POLLHUP) {
					tty_running = 0;
					break;
				}
				try_writing = 1;
			}
		}
		if (try_writing) {
			size_t nwritten = c->srv->interactor->intf->write(c->srv->interactor, &(buf[0]), buf_fill);
			if (nwritten < 0) {
				perror("write to tty");
				tty_running = 0;
				break;
			}
			if (buf_fill == nwritten) {
				buf_fill = 0;
			} else {
				memmove(&(buf[0]), &(buf[nwritten]), buf_fill-nwritten);
				buf_fill -= nwritten;
			}
		}
	}
	if (i_own_the_tty) {
		give_up_tty(c->srv);
		i_own_the_tty = 0;
	}

	close(c->sock);
	close(c->notify_pipe_read);

	pthread_mutex_lock(&(c->srv->clients_lock));
	if (c->prev == NULL) {
		c->srv->clients_head = c->next;
	} else {
		c->prev->next = c->next;
	}
	if (c->next == NULL) {
		c->srv->clients_tail = c->prev;
	} else {
		c->next->prev = c->prev;
	}
	pthread_mutex_unlock(&(c->srv->clients_lock));

	pthread_mutex_lock(&c->input_lock);
	c->input_stop = 1;
	pthread_cond_signal(&c->input_cv);
	pthread_mutex_unlock(&c->input_lock);

	/* It's up to the copy_to_client thread to free the client */

	return NULL;
}

static void
new_card(void *arg, int fd, const char *name)
{
	struct cardserver *srv = (struct cardserver *)arg;

	pthread_t thread_id;
	pthread_attr_t thread_attr;
	int pipefd[2];
	size_t namelen;

	if ((!name) || (!(*name))) {
		close(fd);
		return;
	}
	namelen = strlen(name);
	/* Card names must be .-terminated on the wire. This is because they
	   cannot be empty and .-terminating them is the easiest way to
	   generate appropriate names for the root card on down. But we do
	   not want this dot for presentation. */
	if (name[namelen-1] != '.') {
		close(fd);
		return;
	}

	struct cardclient *c = malloc(sizeof(struct cardclient) + strlen(name));
	if (!c) {
		perror("new_stub: malloc failure");
		close(fd);
		return;
	}
	memset(c, 0, sizeof(*c));
	c->sock = fd;
	c->next = NULL;
	c->srv = srv;
	pthread_mutex_init(&(c->input_lock), NULL);
	pthread_cond_init(&(c->input_cv), NULL);

	if (pipe(&(pipefd[0])) < 0) {
		perror("new_stub: pipe failure");
		close(fd);
		free(c);
		return;
	}
	c->notify_pipe = pipefd[1];
	c->notify_pipe_read = pipefd[0];

	pthread_mutex_lock(&(srv->clients_lock));
	c->card_number = srv->next_card_number++;
	c->prev = srv->clients_tail;
	if (srv->clients_tail) {
		srv->clients_tail->next = c;
	}
	srv->clients_tail = c;
	if (!(srv->clients_head)) {
		srv->clients_head = c;
	}
	pthread_mutex_unlock(&(srv->clients_lock));

	pthread_attr_init(&thread_attr);
	pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);

	pthread_create(&thread_id, &thread_attr, copy_from_client, c);
	pthread_create(&thread_id, &thread_attr, copy_to_client, c);
}

void
new_stub(struct cardserver *srv, int fd)
{
	receive_fds(fd, new_card, srv, NULL);
}

void
card_input(struct cardclient *c, void *data, size_t count)
{
	struct cardinput *i = malloc(sizeof(struct cardinput) + count);
	if (!i) return;
	i->data = &(i[1]);
	if (data != NULL) {
		memcpy(i->data, data, count);
	}
	i->size = count;
	i->next = NULL;

	pthread_mutex_lock(&(c->input_lock));
	if ((c->input_stop) || (c->input_broken)) {
		free(i);
	} else {
		if (c->input_tail) {
			c->input_tail->next = i;
		}
		c->input_tail = i;
		if (!(c->input_head)) {
			c->input_head = i;
		}
		pthread_cond_signal(&c->input_cv);
	}
	pthread_mutex_unlock(&(c->input_lock));
}
