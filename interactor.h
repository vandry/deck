#ifndef _DECK_INTERACTOR_H
#define _DECK_INTERACTOR_H

/* This defines the details of how multiple bytestreams for multiple cards
   are muxed. */

struct pollfd;

struct interactor {
	const struct interactor_interface *intf;
	/* opaque */
};

struct interactor_interface {
	void (*set_input_callback)(
		/* This function is called each time input is received.
		   The interactor will interpret its raw input and
		   demux the identify if the card which the input is for. If
		   the data argument is NULL, it means there is something wrong
		   with the interactor and you should expect no more
		   input. */
		struct interactor *,
		void (*input_callback)(void *data, size_t count,
			int card_number, void *arg),
		void *callback_arg
	);
	/* Further output will be for this card. */
	void (*claim)(struct interactor *, int card_number);
	/* Further output will be for no card. */
	void (*claim_none)(struct interactor *);

	/* Checks if data could be written without blocking.
	   If data is known to be writable without blocking,
	   returns 1 and does not fill in pollfd.
	   If it is not known, returns 0 and fills in pollfd with
	   something that can be polled on. */
	int (*check_ready_for_output)(struct interactor *, struct pollfd *);

	/* Only write after you have claimed! */
	ssize_t (*write)(struct interactor *, const void *buf, size_t count);

	void (*destroy)(struct interactor *);
};

struct interactor *new_interactor(int fd);

#endif /* _DECK_INTERACTOR_H */
