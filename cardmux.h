#ifndef _DECK_CARDMUX_H
#define _DECK_CARDMUX_H

/* Defines the common structures shared by cardserver.[ch] and stub.[ch] */

#include <pthread.h>

struct cardclient {
	struct cardclient *next;
	struct cardclient *prev;
        int card_number;

	/* For other clients to ask for ownership of the tty */
	int notify_pipe;

	/* private */
	struct cardserver *srv;
        int sock;
	int notify_pipe_read;

	pthread_mutex_t input_lock;
	pthread_cond_t input_cv;
	struct cardinput *input_head;
	struct cardinput *input_tail;
	int input_stop;
	int input_broken;
};

struct cardserver {
	struct interactor *interactor;

	pthread_mutex_t clients_lock;
        struct cardclient *clients_head;
        struct cardclient *clients_tail;
	int next_card_number;

	pthread_mutex_t tty_owner_check_lock;
	struct cardclient *tty_owner;

	pthread_mutex_t tty_owner_lock;
	pthread_mutex_t tty_next_owner_lock;

	/* private */
	int master_sock;
};

void claim_tty(struct cardserver *srv, struct cardclient *for_client);
void give_up_tty(struct cardserver *srv);

#endif /* _DECK_CARDMUX_H */
