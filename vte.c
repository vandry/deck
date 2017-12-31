#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <vte/vte.h>
#include <X11/Xlib.h>
#include "interactor.h"

/* This is a dumb sample implementation of the interactor.
   It opens a new GTK+ VTE window for each card.
   It is totally lacking treading synchronization and works by luck!!
   That's it.
*/

struct vte_card {
	struct vte_card *next;
	struct vte_interactor *vtei;
	int card_number;
	GtkWidget *vte;
	GtkWidget *window;
};

struct vte_interactor {
	struct interactor base;
	struct vte_card *cards;
	struct vte_card *active_card;
	int initted;
	void (*input_callback)(void *data, size_t count, int card_number, void *arg);
	void *callback_arg;
};

static void *
run_gtk_main(void *arg)
{
	gtk_main();
	return NULL;
}

static void
vte_init(struct vte_interactor *vtei)
{
	pthread_t thread_id;
	pthread_attr_t thread_attr;

	vtei->initted = 1;

	int argc = 1;
	char **argv = malloc(sizeof(char *) * 2);

	argv[0] = "vtei";
	argv[1] = NULL;

	XInitThreads();
	gtk_init(&argc, &argv);

        pthread_attr_init(&thread_attr);
        pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&thread_id, &thread_attr, run_gtk_main, NULL);
}

static void
commit(VteTerminal *v, gchar *text, guint size, gpointer user_data)
{
	struct vte_card *card = (struct vte_card *)user_data;
	card->vtei->input_callback(text, size, card->card_number, card->vtei->callback_arg);
}

static void
vte_interactor_claim(struct interactor *i, int card_number)
{
	struct vte_interactor *vtei = (struct vte_interactor *)i;
	struct vte_card *card;
	char buf[30];

	if (!vtei->initted) {
		vte_init(vtei);
	}
	for (card = vtei->cards; card; card = card->next) {
		if (card->card_number == card_number) break;
	}
	if (!card) {
		card = malloc(sizeof(*card));
		card->card_number = card_number;
		card->vtei = vtei;

		card->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
		sprintf(buf, "Card #%d", card_number);
		gtk_window_set_title(GTK_WINDOW(card->window), buf);
		card->vte = vte_terminal_new();

		g_signal_connect(card->vte, "commit", G_CALLBACK(commit), card);

		gtk_container_add(GTK_CONTAINER(card->window), card->vte);
		gtk_widget_show_all(card->window);

		card->next = vtei->cards;
		vtei->cards = card;
	}
	vtei->active_card = card;
}

static void
vte_interactor_claim_none(struct interactor *i)
{
	struct vte_interactor *vtei = (struct vte_interactor *)i;
	vtei->active_card = NULL;
}

static ssize_t
vte_interactor_write(struct interactor *i, const void *buf, size_t count)
{
	struct vte_interactor *vtei = (struct vte_interactor *)i;
	vte_terminal_feed(VTE_TERMINAL(vtei->active_card->vte), buf, count);
	return count;
}

static void
vte_interactor_destroy(struct interactor *i)
{
	struct vte_interactor *vtei = (struct vte_interactor *)i;
	while (vtei->cards) {
		struct vte_card *card = vtei->cards;
		vtei->cards = card->next;

		gtk_widget_destroy(card->vte);
		gtk_widget_destroy(card->window);
		free(card);
	}
}

static void
vte_set_input_callback(
	struct interactor *i,
	void (*input_callback)(void *data, size_t count, int card_number, void *arg),
	void *callback_arg
)
{
	struct vte_interactor *vtei = (struct vte_interactor *)i;
	vtei->input_callback = input_callback;
	vtei->callback_arg = callback_arg;
}

static int
vte_interactor_check_ready(struct interactor *i, struct pollfd *pfd)
{
	return 0;
}

const struct interactor_interface vte_interactor_interface = {
	.set_input_callback = vte_set_input_callback,
	.destroy = vte_interactor_destroy,
	.write = vte_interactor_write,
	.claim = vte_interactor_claim,
	.claim_none = vte_interactor_claim_none,
	.check_ready_for_output = vte_interactor_check_ready,
};

struct interactor *
new_interactor(int unused_fd)
{
	struct vte_interactor *vtei = malloc(sizeof(struct vte_interactor));
	if (!vtei) return NULL;
	memset(vtei, 0, sizeof(*vtei));
	vtei->base.intf = &vte_interactor_interface;
	return (struct interactor *)vtei;
}
