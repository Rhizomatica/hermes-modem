CFLAGS=-D_GNU_SOURCE=1 -I. -I../common -I../modem -I../modem/freedv -I../datalink_arq -I../data_interfaces -I../interface -Wall -Wno-strict-overflow -std=gnu11 -fPIC -O2
CC=gcc

.PHONY: all freedv

all: freedv modem.o framer.o

freedv:
	$(MAKE) -C freedv

modem.o: modem.c modem.h
	$(CC) $(CFLAGS) -c modem.c

framer.o: framer.c framer.h
	$(CC) $(CFLAGS) -c framer.c

clean:
	$(MAKE) -C freedv clean
	rm -f *.o
