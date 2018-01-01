#ifndef _DECK_CARDSERVER_H
#define _DECK_CARDSERVER_H

/* A cardserver listens on a socket (master_sock) for connections from
   cardclients. It will allocate a new card to each newly connected client.
   It will multiplex the io from each card onto a single tty (ttyfd).
   initial_client is an already-accepted socket on which an initial client
   (which gets card #0) will be started.
*/

struct cardserver;
struct interactor;

struct cardserver *cardserver(struct interactor *, int initial_client);
void cardserver_quit(struct cardserver *);

#endif /* _DECK_CARDSERVER_H */
