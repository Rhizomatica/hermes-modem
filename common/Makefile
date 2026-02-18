include ../config.mk

CFLAGS = $(COMMON_CFLAGS)

all: os_interop.o ring_buffer_posix.o shm_posix.o crc6.o hermes_log.o chan.o queue.o

os_interop.o: os_interop.c
	$(CC) $(CFLAGS) -c os_interop.c

ring_buffer_posix.o: ring_buffer_posix.c
	$(CC) $(CFLAGS) -c ring_buffer_posix.c

shm_posix.o: shm_posix.c
	$(CC) $(CFLAGS) -c shm_posix.c

crc6.o: crc6.c
	$(CC) $(CFLAGS) -c crc6.c

hermes_log.o: hermes_log.c hermes_log.h
	$(CC) $(CFLAGS) -c hermes_log.c

chan.o: chan.c chan.h queue.h
	$(CC) $(CFLAGS) -c chan.c

queue.o: queue.c queue.h
	$(CC) $(CFLAGS) -c queue.c

.PHONY: all clean

clean:
	rm -f *.o
