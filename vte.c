#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <alloca.h>
#include <vte/vte.h>
#include <X11/Xlib.h>
#include "renderer.h"

/* This is a dumb sample implementation of the renderer.
   It opens a new GTK+ VTE window for each card.
   It is totally lacking treading synchronization and works by luck!!
   That's it.
*/

struct vte_card {
	struct vte_card *next;
	struct vte_renderer *vtei;
	const char *card_name;
	GtkWidget *vte;
	GtkWidget *window;
};

struct vte_renderer {
	struct renderer base;
	struct vte_card *cards;
	struct vte_card *active_card;
	int initted;
	void (*input_callback)(void *data, size_t count, const char *card_name, void *arg);
	void *callback_arg;
};

static void *
run_gtk_main(void *arg)
{
	gtk_main();
	return NULL;
}

static void
vte_init(struct vte_renderer *vtei)
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
	card->vtei->input_callback(text, size, card->card_name, card->vtei->callback_arg);
}

static void
vte_renderer_claim(struct renderer *i, const char *card_name)
{
	struct vte_renderer *vtei = (struct vte_renderer *)i;
	struct vte_card *card;
	char *buf = alloca(strlen(card_name) + 10);

	if (!vtei->initted) {
		vte_init(vtei);
	}
	for (card = vtei->cards; card; card = card->next) {
		if (0 == strcmp(card->card_name, card_name)) break;
	}
	if (!card) {
		card = malloc(sizeof(*card) + strlen(card_name) + 1);
		card->card_name = (const char *)(&(card[1]));
		strcpy((char *)(&(card[1])), card_name);
		card->vtei = vtei;

		card->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
		sprintf(buf, "Card \"%s\"", card_name);
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
vte_renderer_claim_none(struct renderer *i)
{
	struct vte_renderer *vtei = (struct vte_renderer *)i;
	vtei->active_card = NULL;
}

static ssize_t
vte_renderer_write(struct renderer *i, const void *buf, size_t count)
{
	struct vte_renderer *vtei = (struct vte_renderer *)i;
	vte_terminal_feed(VTE_TERMINAL(vtei->active_card->vte), buf, count);
	return count;
}

static void
vte_renderer_destroy(struct renderer *i)
{
	struct vte_renderer *vtei = (struct vte_renderer *)i;
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
	struct renderer *i,
	void (*input_callback)(void *data, size_t count, const char *card_name, void *arg),
	void *callback_arg
)
{
	struct vte_renderer *vtei = (struct vte_renderer *)i;
	vtei->input_callback = input_callback;
	vtei->callback_arg = callback_arg;
}

static int
vte_renderer_check_ready(struct renderer *i, struct pollfd *pfd)
{
	return 0;
}

const struct renderer_interface vte_renderer_interface = {
	.set_input_callback = vte_set_input_callback,
	.destroy = vte_renderer_destroy,
	.write = vte_renderer_write,
	.claim = vte_renderer_claim,
	.claim_none = vte_renderer_claim_none,
	.check_ready_for_output = vte_renderer_check_ready,
};

struct renderer *
new_renderer(int unused_fd)
{
	struct vte_renderer *vtei = malloc(sizeof(struct vte_renderer));
	if (!vtei) return NULL;
	memset(vtei, 0, sizeof(*vtei));
	vtei->base.intf = &vte_renderer_interface;
	return (struct renderer *)vtei;
}
