#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "cardserver.h"
#include "cardmux.h"
#include "stub.h"
#include "util.h"
#include "interactor.h"

void
claim_tty(struct cardserver *srv, struct cardclient *c)
{
	struct cardclient *other_client;
	const char *dummy = "1";

	pthread_mutex_lock(&(srv->tty_next_owner_lock));

	pthread_mutex_lock(&(srv->tty_owner_check_lock));
	other_client = srv->tty_owner;
	if (other_client) {
		write(other_client->notify_pipe, &dummy, 1);
	}
	pthread_mutex_unlock(&(srv->tty_owner_check_lock));

	pthread_mutex_lock(&(srv->tty_owner_lock));
	pthread_mutex_lock(&(srv->tty_owner_check_lock));
	srv->tty_owner = c;
	pthread_mutex_unlock(&(srv->tty_owner_check_lock));

	pthread_mutex_unlock(&(srv->tty_next_owner_lock));

	if (c != NULL) {
		srv->interactor->intf->claim(srv->interactor, c->card_number);
	}
}

void
give_up_tty(struct cardserver *srv)
{
	srv->interactor->intf->claim_none(srv->interactor);

	pthread_mutex_lock(&(srv->tty_owner_check_lock));
	srv->tty_owner = NULL;
	pthread_mutex_unlock(&(srv->tty_owner_check_lock));

	pthread_mutex_unlock(&(srv->tty_owner_lock));
}

void
cardserver_quit(struct cardserver *srv)
{
	/* Force anything that already has the tty to give it up */
	claim_tty(srv, NULL);
	srv->interactor->intf->destroy(srv->interactor);
}

static void
input_callback(void *data, size_t count, int card_number, void *arg)
{
	struct cardserver *srv = (struct cardserver *)arg;
	struct cardclient *card;

	pthread_mutex_lock(&(srv->clients_lock));
	for (card = srv->clients_head; card; card = card->next) {
		if (card->card_number == card_number) break;
	}
	if (card) {
		card_input(card, data, count);
	}
	pthread_mutex_unlock(&(srv->clients_lock));

	if (!card) {
		fprintf(stderr, "Input for unknown card %d\n", card_number);
	}
}

static void *
acceptor(void *arg)
{
	struct cardserver *srv = (struct cardserver *)arg;
	struct sockaddr_un sa;
	socklen_t salen;
	int s;

	for (;;) {
		salen = sizeof(sa);
		s = accept(srv->master_sock, (struct sockaddr *)(&sa), &salen);
		if (s < 0) {
			perror("cardserver accept");
			sleep(1);
			continue;
		}
		new_stub(srv, s);
	}
	return NULL;
}

struct cardserver *
cardserver(struct interactor *interactor, int master_sock, int initial_client)
{
	struct cardserver *srv;
	pthread_t thread_id;
	pthread_attr_t thread_attr;

	srv = malloc(sizeof(*srv));
	if (!srv) {
		perror("cardserver startup: malloc failed");
		return NULL;
	}
	memset(srv, 0, sizeof(*srv));
	pthread_mutex_init(&(srv->clients_lock), NULL);
	srv->interactor = interactor;
	interactor->intf->set_input_callback(interactor, input_callback, srv);
	pthread_mutex_init(&(srv->tty_owner_check_lock), NULL);
	pthread_mutex_init(&(srv->tty_owner_lock), NULL);
	pthread_mutex_init(&(srv->tty_next_owner_lock), NULL);

	new_stub(srv, initial_client);

	listen(master_sock, 15);
	srv->master_sock = master_sock;
	pthread_attr_init(&thread_attr);
	pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
	pthread_create(&thread_id, &thread_attr, acceptor, srv);

	return srv;
}
