#ifndef _DECK_UTIL_H
#define _DECK_UTIL_H

/* returns -1 if stdio is not connected to our tty or we have no tty */
/* returns a fd to our tty otherwise. */
/* In the latter case, stdio_is_my_tty[0] says if stdio is attached to
   the tty, stdio_is_my_tty[1] says if stdout is attached to the tty,
   and stdio_is_my_tty[2] says if stderr is. */
int stdio_connected_to_tty(int *stdio_is_my_tty);

void setnonblock(int fd);

/* Launch a thread to receive fds on a socket, one at a time. Each time
   one is received, the callback is called with the new fd and the byte
   data that came with it, NUL-terminated. */
void receive_fds(int fd,
	void (*callback)(void *arg, int fd, const char *data), void *callback_arg,
	pthread_t *ret_thread_id
);

#endif /* _DECK_UTIL_H */
