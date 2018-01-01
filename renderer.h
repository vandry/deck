#ifndef _DECK_RENDERER_H
#define _DECK_RENDERER_H

/* This defines the details of how multiple bytestreams for multiple cards
   are muxed. */

struct pollfd;

struct renderer {
	const struct renderer_interface *intf;
	/* opaque */
};

struct renderer_interface {
	void (*set_input_callback)(
		/* This function is called each time input is received.
		   The renderer will interpret its raw input and
		   demux the identify if the card which the input is for. If
		   the data argument is NULL, it means there is something wrong
		   with the renderer and you should expect no more
		   input. */
		struct renderer *,
		void (*input_callback)(void *data, size_t count,
			const char *card_name, void *arg),
		void *callback_arg
	);
	/* Further output will be for this card. */
	void (*claim)(
		struct renderer *,
		/* The renderer may hang on to card_name without copying it
		   and compare the pointer by identity. The caller is not
		   to free it or change it until the claim is relinquished. */
		const char *card_name
	);
	/* Further output will be for no card. */
	void (*claim_none)(struct renderer *);

	/* Checks if data could be written without blocking.
	   If data is known to be writable without blocking,
	   returns 1 and does not fill in pollfd.
	   If it is not known, returns 0 and fills in pollfd with
	   something that can be polled on. */
	int (*check_ready_for_output)(struct renderer *, struct pollfd *);

	/* Only write after you have claimed! */
	ssize_t (*write)(struct renderer *, const void *buf, size_t count);

	void (*destroy)(struct renderer *);
};

struct renderer *new_renderer(int fd);

#endif /* _DECK_RENDERER_H */
