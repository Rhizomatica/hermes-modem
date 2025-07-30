CC = gcc
CFLAGS = -Wall -O2 -std=gnu11 -I../src

all: ../modem

../modem: main.o ../src/libfreedvdata.a
	$(CC) -o ../modem main.o ../src/libfreedvdata.a -lm

main.o: main.c
	$(CC) $(CFLAGS) -c main.c

clean:
	rm -f *.o
