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

include config.mk

.PHONY: all internal_deps clean

CFLAGS = $(COMMON_CFLAGS) -Imodem/freedv -Imodem -Idatalink_broadcast -Idata_interfaces -Idatalink_arq -Iaudioio/ffaudio -Icommon

LDFLAGS=$(FFAUDIO_LINKFLAGS) -lm

all: mercury

mercury: internal_deps main.o 
	$(CC) -o mercury  \
		main.o datalink_arq/arq.o datalink_arq/fsm.o datalink_arq/arith.o datalink_broadcast/broadcast.o modem/modem.o \
		modem/framer.o modem/freedv/libfreedvdata.a audioio/audioio.a common/os_interop.o common/ring_buffer_posix.o common/shm_posix.o \
		common/crc6.o data_interfaces/tcp_interfaces.o data_interfaces/net.o $(LDFLAGS)

main.o: main.c
	$(CC) $(CFLAGS) -c main.c


internal_deps:
	$(MAKE) -C modem
	$(MAKE) -C datalink_arq
	$(MAKE) -C datalink_broadcast
	$(MAKE) -C data_interfaces
	$(MAKE) -C audioio
	$(MAKE) -C common


clean:
	rm -f mercury *.o
	$(MAKE) -C modem clean
	$(MAKE) -C datalink_arq clean
	$(MAKE) -C datalink_broadcast clean
	$(MAKE) -C data_interfaces clean
	$(MAKE) -C audioio clean
	$(MAKE) -C common clean
