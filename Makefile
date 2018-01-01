CC=gcc
CFLAGS=-Wall -Wno-parentheses -g -pthread

DECK_OBJS=deck.o util.o cardclient.o cardserver.o stub.o
CARD_OBJS=card.o cardclient.o util.o
ALL_OBJS=deck.o util.o cardclient.o cardserver.o stub.o tty.o vte.o

all: deck vtedeck card

clean:
	rm -f $(ALL_OBJS) deck vtedeck card

deck.o: deck.c util.h cardclient.h cardserver.h renderer.h

util.o: util.c util.h

cardclient.o: cardclient.c cardclient.h util.h global.h

cardserver.o: cardserver.c cardserver.h cardmux.h stub.h util.h renderer.h

stub.o: stub.c cardmux.h stub.h util.h renderer.h

tty.o: tty.c renderer.h util.h

card.o: card.c cardclient.h global.h

vte.o: vte.c renderer.h
	$(CC) -c $(CFLAGS) `pkg-config --cflags vte` -o $@ vte.c

deck: $(DECK_OBJS) tty.o
	$(CC) $(CFLAGS) -o $@ $(DECK_OBJS) tty.o -lutil

vtedeck: $(DECK_OBJS) vte.o
	$(CC) $(CFLAGS) -o $@ $(DECK_OBJS) vte.o -lutil `pkg-config --libs vte`

card: $(CARD_OBJS)
	$(CC) $(CFLAGS) -o $@ $(CARD_OBJS) -lutil
