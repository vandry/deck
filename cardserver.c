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
#include "renderer.h"

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
		srv->renderer->intf->claim(srv->renderer, c->card_name);
	}
}

void
give_up_tty(struct cardserver *srv)
{
	srv->renderer->intf->claim_none(srv->renderer);

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
	srv->renderer->intf->destroy(srv->renderer);
}

static void
input_callback(void *data, size_t count, const char *card_name, void *arg)
{
	struct cardserver *srv = (struct cardserver *)arg;
	struct cardclient *card;

	pthread_mutex_lock(&(srv->clients_lock));
	for (card = srv->clients_head; card; card = card->next) {
		if (0 == strcmp(card_name, card->card_name)) break;
	}
	if (card) {
		card_input(card, data, count);
	}
	pthread_mutex_unlock(&(srv->clients_lock));

	if (!card) {
		fprintf(stderr, "Input for unknown card \"%s\"\n", card_name);
	}
}

struct cardserver *
cardserver(struct renderer *renderer, int initial_client)
{
	struct cardserver *srv;

	srv = malloc(sizeof(*srv));
	if (!srv) {
		perror("cardserver startup: malloc failed");
		return NULL;
	}
	memset(srv, 0, sizeof(*srv));
	pthread_mutex_init(&(srv->clients_lock), NULL);
	srv->renderer = renderer;
	renderer->intf->set_input_callback(renderer, input_callback, srv);
	pthread_mutex_init(&(srv->tty_owner_check_lock), NULL);
	pthread_mutex_init(&(srv->tty_owner_lock), NULL);
	pthread_mutex_init(&(srv->tty_next_owner_lock), NULL);

	new_stub(srv, initial_client);

	return srv;
}
