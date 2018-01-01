#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <pty.h>
#include <string.h>
#include <alloca.h>
#include <termios.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include "global.h"
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

struct socket_param {
	char *socket_dir;
	char *socket_name;
	char *env_var;
};

static int
setup_socket(struct socket_param *p)
{
	int fd;
	char socket_dir_template[30];
	char *socket_dir;
	struct sockaddr_un socket_name;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}
	sprintf(socket_dir_template, "/tmp/carddeck.XXXXXX");
	socket_dir = mkdtemp(socket_dir_template);
	if (!socket_dir) {
		perror("mkdtemp");
		return -1;
	}
	memset(&socket_name, 0, sizeof(socket_name));
	socket_name.sun_family = AF_UNIX;
	sprintf(socket_name.sun_path, "%s/sock", socket_dir);
	if (bind(fd, (struct sockaddr*)&socket_name, sizeof(socket_name)) < 0) {
		perror("bind master socket");
giveup:
		close(fd);
		unlink(socket_name.sun_path);
		rmdir(socket_dir);
		return -1;
	}
	if (listen(fd, 15) < 0) {
		perror("listen");
		goto giveup;
	}

	p->env_var = malloc(sizeof(CARDDECK_SOCKET_VAR_NAME) + 3 + strlen(socket_name.sun_path));
	if (!(p->env_var)) {
		perror("malloc");
		goto giveup;
	}
	sprintf(p->env_var, CARDDECK_SOCKET_VAR_NAME "=%s", socket_name.sun_path);
	p->socket_name = p->env_var + sizeof(CARDDECK_SOCKET_VAR_NAME);
	p->socket_dir = strdup(socket_dir);
	if (!(p->socket_dir)) {
		perror("malloc");
		free(p->env_var);
		goto giveup;
	}
	return fd;
}

static void
cleanup_socket(struct socket_param *p)
{
	unlink(p->socket_name);
	rmdir(p->socket_dir);
	free(p->env_var);
	free(p->socket_dir);
}

static int
pass_card(int upperdeck, int fd, const char *cardname)
{
	struct msghdr msg = { 0 };
	char buf[CMSG_SPACE(sizeof(int))];
	struct iovec io;
	struct cmsghdr *cmsg;

	memset(buf, 0, sizeof(buf));
	io.iov_base = (void *)cardname;
	io.iov_len = strlen(cardname);
	msg.msg_iov = &io;
	msg.msg_iovlen = 1;
	msg.msg_control = buf;
	msg.msg_controllen = sizeof(buf);

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	*((int *)CMSG_DATA(cmsg)) = fd;
	msg.msg_controllen = cmsg->cmsg_len;

	if (sendmsg(upperdeck, &msg, 0) < 0) {
		perror("sendmsg");
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

static int
make_card(int upperdeck, const char *cardname)
{
	int sv[2];

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, &(sv[0])) < 0) {
		perror("socketpair");
		return -1;
	}
	int ret = pass_card(upperdeck, sv[0], cardname);
	if (ret < 0) {
		close(sv[1]);
		return -1;
	}
	return sv[1];
}

struct card_receiver {
	struct card_receiver *next;
	pthread_t self;
	int upperdeck;
	int card_number;
};

static void
accept_card(void *arg, int fd, const char *name_in)
{
	struct card_receiver *r = (struct card_receiver *)arg;
	size_t namelen = strlen(name_in);
	if ((namelen < 1) || (name_in[namelen-1] != '.') || (name_in[0] != '.')) {
		close(fd);
		return;
	}
	char *name_out = alloca(strlen(name_in) + 15);
	sprintf(name_out, ".%d.%s", r->card_number, name_in+1);
	(void)pass_card(r->upperdeck, fd, name_out);
}

struct acceptor_args {
	int master_socket;
	int upperdeck;
	int next_card_number;
	struct card_receiver *card_receivers;
};

static void
acceptor_cleanup(void *arg)
{
	struct acceptor_args *a = (struct acceptor_args *)arg;
	void *unused;

	close(a->master_socket);
	while (a->card_receivers) {
		struct card_receiver *r = a->card_receivers;
		a->card_receivers = r->next;
		pthread_cancel(r->self);
		pthread_join(r->self, &unused);
	}
}

static void *
acceptor(void *arg)
{
	struct acceptor_args *a = (struct acceptor_args *)arg;
	struct sockaddr_un sa;
	socklen_t salen;
	int s;
	struct card_receiver *receiver;

	pthread_cleanup_push(acceptor_cleanup, arg);
	for (;;) {
		salen = sizeof(sa);
		s = accept(a->master_socket, (struct sockaddr *)(&sa), &salen);
		if (s < 0) {
			perror("cardserver accept");
			break;
		}
		receiver = malloc(sizeof(*receiver));
		if (!receiver) {
			perror("malloc");
			close(s);
			continue;
		}
		memset(receiver, 0, sizeof(*receiver));
		receiver->card_number = a->next_card_number++;
		receiver->upperdeck = a->upperdeck;
		receiver->next = a->card_receivers;
		a->card_receivers = receiver;
		receive_fds(s, accept_card, receiver, &(receiver->self));
	}
	pthread_cleanup_pop(1);
	return NULL;
}

int
cardclient(int sock_to_cardserver, int *stdio_is_tty, struct tty_settings *ts,
	int extra_fd_to_close_in_child, char **argv)
{
	int ptymaster, ptyslave, root_card;
	pid_t child;
	int i;
	int notify_pipe[2];
	pthread_t waitpid_thread, acceptor_thread;
	struct waitpid_thread_args waitpid_thread_args;
	struct socket_param socket_param;
	struct acceptor_args acceptor_args;
	int master_socket;
	void *unused;

	root_card = make_card(sock_to_cardserver, ".");
	if (root_card < 0) {
		return 1;
	}

	master_socket = setup_socket(&socket_param);
	if (master_socket >= 0) {
		memset(&acceptor_args, 0, sizeof(acceptor_args));
		acceptor_args.master_socket = master_socket;
		acceptor_args.upperdeck = sock_to_cardserver;
		pthread_create(&acceptor_thread, NULL, acceptor, &acceptor_args);
	}

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
		close(sock_to_cardserver);
		close(root_card);
		if (master_socket >= 0) {
			close(master_socket);
			putenv(socket_param.env_var);
		} else {
			putenv(CARDDECK_SOCKET_VAR_NAME "=(error)");
		}
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
	childio(root_card, ptymaster, notify_pipe[0]);

	if (master_socket >= 0) {
		pthread_cancel(acceptor_thread);
	}
	close(root_card);
	close(ptymaster);
	if (notify_pipe[0] != -1) {
		close(notify_pipe[0]);
	}
	cleanup_socket(&socket_param);
	pthread_join(waitpid_thread, &unused);
	if (master_socket >= 0) {
		pthread_join(acceptor_thread, &unused);
	}
	close(sock_to_cardserver);

	return WEXITSTATUS(waitpid_thread_args.status);
}
