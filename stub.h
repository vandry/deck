#ifndef _DECK_STUB_H
#define _DECK_STUB_H

/* Defines the bit that accepts io from connected cardclients and interacts
   with cardserver to mux this io. Only cardserver calls here. */

/* Create a new cardclient stub on the given server. The fd is a socket
   that is already accepted. */
void new_stub(struct cardserver *, int fd);

/* Some input has been received for this card. Send it out to the
   cardclient through and socket. */
void card_input(struct cardclient *, void *data, size_t count);

#endif /* _DECK_STUB_H */
