CC = gcc
CFLAGS = -Wall -O2 -std=gnu11 -pthread -D_GNU_SOURCE -I../src -I.. -I../audioio/ffbase -I../common
LDFLAGS = -lm -lpthread -lpulse -lasound

all: ../modem

../modem: main.o ../src/libfreedvdata.a ../audioio/audioio.a ../common/os_interop.o ../common/ring_buffer_posix.o ../common/shm_posix.o
	$(CC) -o ../modem main.o ../src/libfreedvdata.a ../audioio/audioio.a ../common/os_interop.o ../common/ring_buffer_posix.o ../common/shm_posix.o $(LDFLAGS)

main.o: main.c
	$(CC) $(CFLAGS) -c main.c

clean:
	rm -f *.o
