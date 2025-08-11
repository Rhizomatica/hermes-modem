# HERMES Modem
#
# Copyright (C) 2024-2025 Rhizomatica
# Author: Rafael Diniz <rafael@riseup.net>
#
# This is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3, or (at your option)
# any later version.
#
# This software is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this software; see the file COPYING.  If not, write to
# the Free Software Foundation, Inc., 51 Franklin Street,
# Boston, MA 02110-1301, USA.
#

ifeq ($(OS),Windows_NT)
	FFAUDIO_LINKFLAGS += -lole32
	FFAUDIO_LINKFLAGS += -ldsound -ldxguid
	FFAUDIO_LINKFLAGS += -lws2_32
	FFAUDIO_LINKFLAGS += -static-libgcc -static-libstdc++ -static -l:libwinpthread.a
else
	UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Linux)
		FFAUDIO_LINKFLAGS += -lpulse
		FFAUDIO_LINKFLAGS += -lasound -lpthread -lrt
    endif
    ifeq ($(UNAME_S),Darwin)
		FFAUDIO_LINKFLAGS := -framework CoreFoundation -framework CoreAudio
    endif
    ifeq ($(UNAME_S),FreeBSD)
		FFAUDIO_LINKFLAGS := -lm
    endif
endif


CC = gcc
CFLAGS = -Wall -O2 -std=gnu11 -pthread -D_GNU_SOURCE -I../modem/freedv -I.. -I../audioio/ffaudio -I../common

LDFLAGS=$(FFAUDIO_LINKFLAGS) -lm

all: ../mercury

../mercury: main.o arq.o net.o fsm.o arith.o crc6.o modem.o broadcast.o ../modem/freedv/libfreedvdata.a ../audioio/audioio.a \
		../common/os_interop.o ../common/ring_buffer_posix.o ../common/shm_posix.o
	$(CC) -o ../mercury  \
		main.o arq.o net.o fsm.o arith.o crc6.o modem.o broadcast.o ../modem/freedv/libfreedvdata.a ../audioio/audioio.a \
		../common/os_interop.o ../common/ring_buffer_posix.o ../common/shm_posix.o $(LDFLAGS)

main.o: main.c
	$(CC) $(CFLAGS) -c main.c

arq.o: arq.c
	$(CC) $(CFLAGS) -c arq.c

net.o: net.c
	$(CC) $(CFLAGS) -c net.c

fsm.o: fsm.c
	$(CC) $(CFLAGS) -c fsm.c

arith.o: arith.c
	$(CC) $(CFLAGS) -c arith.c

crc6.o: crc6.c
	$(CC) $(CFLAGS) -c crc6.c

modem.o: modem.c
	$(CC) $(CFLAGS) -c modem.c

broadcast.o: broadcast.c
	$(CC) $(CFLAGS) -c broadcast.c

clean:
	rm -f *.o
