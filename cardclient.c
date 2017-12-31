#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <pty.h>
#include <string.h>
#include <termios.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include "cardclient.h"
#include "util.h"

static void
maybe_write(size_t *tocopyp, char *buf, short revents, int fd)
{
	if ((*tocopyp) == 0) {
		return;
	}
	if (!(revents & POLLOUT)) {
		return;
	}
	ssize_t written = write(fd, buf, *tocopyp);
	if (written <= 0) {
		return;
	}
	if (written == (*tocopyp)) {
		*tocopyp = 0;  /* wrote everything */
	} else {
		memmove(buf, buf+written, (*tocopyp) - written);
		*tocopyp -= written;
	}
}

static int
maybe_read(size_t *tocopyp, char *buf, size_t buf_size, short revents, int fd)
{
	if ((*tocopyp) >= buf_size) {
		return 0;
	}
	if (!(revents & POLLIN)) {
		return 0;
	}
	ssize_t nread = read(fd, buf + *tocopyp, buf_size - (*tocopyp));
	if (nread > 0) {
		*tocopyp += nread;
	}
	return nread <= 0;
}

static void
childio(int fd0, int fd1, int quit_pipe)
{
	struct pollfd pollfd[3];
	int nfds = (quit_pipe == -1) ? 2 : 3;
	char buf0to1[4096];
	size_t tocopy_0to1 = 0;
	char buf1to0[4096];
	size_t tocopy_1to0 = 0;
	int err = 0;

	pollfd[0].fd = fd0;
	pollfd[1].fd = fd1;
	pollfd[2].fd = quit_pipe;

	setnonblock(fd0);
	setnonblock(fd1);
	setnonblock(quit_pipe);

	while (!err) {
		pollfd[0].events =
			/* Space available to read? */
			((tocopy_0to1 < sizeof(buf0to1)) ? POLLIN : 0) |
			/* Things need to be written? */
			((tocopy_1to0 > 0) ? POLLOUT : 0);
		pollfd[1].events =
			/* Space available to read? */
			((tocopy_1to0 < sizeof(buf1to0)) ? POLLIN : 0) |
			/* Things need to be written? */
			((tocopy_0to1 > 0) ? POLLOUT : 0);
		pollfd[2].events = POLLIN;
		if (poll(&(pollfd[0]), nfds, -1) <= 0) {
			if (errno == EAGAIN) continue;
			if (errno == EINTR) continue;
			sleep(1);
			continue;
		}

		if ((pollfd[0].revents & POLLHUP) || (pollfd[1].revents & POLLHUP)) {
			break;
		}
		if ((nfds > 2) && (pollfd[2].revents)) {
			/* The child has died, but there might be grandchildren
			   still going. We can continue, but we have to get out
			   of the way of the process parentage chain. */
			if (fork() == 0) {
				close(quit_pipe);
				childio(fd0, fd1, -1);
				_exit(0);
			}
			break;
		}
		maybe_write(&tocopy_1to0, buf1to0, pollfd[0].revents, fd0);
		maybe_write(&tocopy_0to1, buf0to1, pollfd[1].revents, fd1);
		err |= maybe_read(&tocopy_0to1, buf0to1, sizeof(buf0to1), pollfd[0].revents, fd0);
		err |= maybe_read(&tocopy_1to0, buf1to0, sizeof(buf1to0), pollfd[1].revents, fd1);
	}
}

struct waitpid_thread_args {
	pid_t pid;
	int notify_fd;
	int status;
};

static void *
do_waitpid(void *varg)
{
	struct waitpid_thread_args *arg = (struct waitpid_thread_args *)varg;

	for (;;) {
		if (waitpid(arg->pid, &(arg->status), 0) < 0) {
			if (errno == EINTR) continue;
			if (errno == EAGAIN) continue;
			sleep(1);
			continue;
		}
		if (WIFEXITED(arg->status)) break;
	}
	close(arg->notify_fd);
	return varg;
}

void
collect_tty_settings(int ttyfd, struct tty_settings *ts)
{
	ts->attrsp = NULL;
	ts->winp = NULL;
	if (tcgetattr(ttyfd, &(ts->attrs)) == 0) {
		ts->attrsp = &(ts->attrs);
	}
	if (ioctl(ttyfd, TIOCGWINSZ, (char *)(&(ts->win))) == 0) {
		ts->winp = &(ts->win);
	}
}


int
cardclient(int sock_to_cardserver, int *stdio_is_tty, struct tty_settings *ts,
	int extra_fd_to_close_in_child, char **argv)
{
	int ptymaster, ptyslave;
	pid_t child;
	int i;
	int notify_pipe[2];
	pthread_t waitpid_thread;
	struct waitpid_thread_args waitpid_thread_args;
	void *unused;

	if (openpty(&ptymaster, &ptyslave, NULL, ts->attrsp, ts->winp) < 0) {
		perror("openpty");
		return 1;
	}
	child = fork();
	if (child < 0) {
		perror("fork");
		close(ptymaster);
		close(ptyslave);
		return 1;
	}
	if (child == 0) {
		close(ptymaster);
		if (extra_fd_to_close_in_child != -1) {
			close(extra_fd_to_close_in_child);
		}
		for (i = 0; i < 3; i++) {
			/* At least one of these ought to always be set. */
			if (!stdio_is_tty[i]) {
				continue;
			}
			dup2(ptyslave, i);
		}
		setsid();
		ioctl(ptyslave, TIOCSCTTY, 0);
		close(ptyslave);
		execvp(argv[0], argv);
		perror("exec");
		_exit(1);
	}
	close(ptyslave);

	if (pipe(&(notify_pipe[0])) == 0) {
		waitpid_thread_args.pid = child;
		waitpid_thread_args.notify_fd = notify_pipe[1];
		pthread_create(&waitpid_thread, NULL, do_waitpid, &waitpid_thread_args);
	} else {
		/* I guess we are not going to watch the child! */
		notify_pipe[0] = -1;
	}

	/* Just copy, but also wait for the child. */
	childio(sock_to_cardserver, ptymaster, notify_pipe[0]);

	close(sock_to_cardserver);
	close(ptymaster);
	if (notify_pipe[0] != -1) {
		close(notify_pipe[0]);
	}

	pthread_join(waitpid_thread, &unused);
	return WEXITSTATUS(waitpid_thread_args.status);
}
