include ../config.mk

CFLAGS = $(COMMON_CFLAGS)

all: os_interop.o ring_buffer_posix.o shm_posix.o crc6.o

os_interop.o: os_interop.c
	$(CC) $(CFLAGS) -c os_interop.c

ring_buffer_posix.o: ring_buffer_posix.c
	$(CC) $(CFLAGS) -c ring_buffer_posix.c

shm_posix.o: shm_posix.c
	$(CC) $(CFLAGS) -c shm_posix.c

crc6.o: crc6.c
	$(CC) $(CFLAGS) -c crc6.c

.PHONY: all clean

clean:
	rm -f *.o
