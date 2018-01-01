#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include "util.h"

int
stdio_connected_to_tty(int *stdio_is_my_tty)
{
	int i;
	int ttyfd = -1;

	stdio_is_my_tty[0] = stdio_is_my_tty[1] = stdio_is_my_tty[2] = 0;
	for (i = 0; i < 3; i++) {
		if (isatty(i)) {
			stdio_is_my_tty[i] = 1;
			if (ttyfd == -1) {
				ttyfd = i;
			}
		}
	}
	if (!(stdio_is_my_tty[0] || stdio_is_my_tty[1] || stdio_is_my_tty[2])) {
		return -1;
	}
	return ttyfd;
}

void
setnonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL);
	if (flags == -1) {
		return;
	}
	flags |= O_NONBLOCK;
	fcntl(fd, F_SETFL, flags);
}

struct fd_receiver {
	void (*callback)(void *arg, int fd, const char *data);
	void *callback_arg;
	int fd;
};

static void fd_receiver_cleanup(void *arg)
{
	struct fd_receiver *r = (struct fd_receiver *)arg;
	close(r->fd);
	free(r);
}

static void *
run_fd_receiver(void *arg)
{
	struct fd_receiver *r = (struct fd_receiver *)arg;
	struct msghdr msg;
	struct cmsghdr *cmsg;
	struct iovec io;
	ssize_t n;
	int card_fd;
	char m_buffer[4096];
	char c_buffer[256];

	pthread_cleanup_push(fd_receiver_cleanup, arg);
	for (;;) {
		io.iov_base = m_buffer;
		io.iov_len = sizeof(m_buffer) - 1;
		memset(&msg, 0, sizeof(msg));
		msg.msg_iov = &io;
		msg.msg_iovlen = 1;
		msg.msg_control = c_buffer;
		msg.msg_controllen = sizeof(c_buffer);

		n = recvmsg(r->fd, &msg, 0);
		if (n == 0) {
			break;
		}
		if (n < 0) {
			if (errno == EAGAIN) continue;
			if (errno == EINTR) continue;
			perror("recvmsg");
			break;
		}

		cmsg = CMSG_FIRSTHDR(&msg);
		memmove(&card_fd, CMSG_DATA(cmsg), sizeof(card_fd));
		m_buffer[n] = 0;
		r->callback(r->callback_arg, card_fd, m_buffer);
	}
	pthread_cleanup_pop(1);
	return NULL;
}

void
receive_fds(int fd, void (*callback)(void *arg, int fd, const char *data), void *callback_arg, pthread_t *thread_id)
{
	pthread_t unused_thread_id;
	pthread_attr_t thread_attr;
	struct fd_receiver *r = malloc(sizeof(struct fd_receiver));

	if (!r) {
		perror("receive_fds: malloc");
		close(fd);
		return;
	}
	r->callback = callback;
	r->callback_arg = callback_arg;
	r->fd = fd;

	pthread_attr_init(&thread_attr);
	if (thread_id == NULL) {
		/* Caller wants to forget the thread. */
		pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
		thread_id = &unused_thread_id;
	}
	pthread_create(thread_id, &thread_attr, run_fd_receiver, r);
}
