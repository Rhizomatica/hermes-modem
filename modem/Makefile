CFLAGS=-D_GNU_SOURCE=1 -I. -I../common -I../modem -I../modem/freedv -I../datalink_arq -Wall -Wno-strict-overflow -std=gnu11 -fPIC -O2
CC=gcc

.PHONY: all freedv 

all: freedv modem.o

freedv:
	$(MAKE) -C freedv

modem.o: modem.c modem.h
	$(CC) $(CFLAGS) -c modem.c

clean:
	$(MAKE) -C freedv clean
	rm -f *.o
