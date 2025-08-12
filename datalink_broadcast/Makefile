CC=gcc
CFLAGS=-Wall -O2 -std=gnu11 -pthread -D_GNU_SOURCE=1 -I../modem -I../modem/freedv -I../common -I../datalink_arq
all: broadcast.o kiss.o

kiss.o: kiss.c
	$(CC) $(CFLAGS) -c kiss.c

broadcast.o: broadcast.c
	$(CC) $(CFLAGS) -c broadcast.c


clean:
	rm *.o
