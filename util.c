#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
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
