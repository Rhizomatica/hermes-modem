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

.PHONY: all internal_deps utils clean

CFLAGS = $(COMMON_CFLAGS) -Imodem/freedv -Imodem -Idatalink_broadcast -Idata_interfaces -Idatalink_arq -Iaudioio/ffaudio -Icommon -Ithird_party/chan

LDFLAGS=$(FFAUDIO_LINKFLAGS) -lm

MERCURY_LINK_INPUTS = \
	main.o datalink_arq/arq.o datalink_arq/arq_v2.o datalink_arq/fsm.o datalink_arq/arith.o datalink_arq/arq_channels.o \
	datalink_broadcast/broadcast.o datalink_broadcast/kiss.o modem/modem.o modem/framer.o modem/freedv/libfreedvdata.a \
	audioio/audioio.a common/os_interop.o common/ring_buffer_posix.o common/shm_posix.o common/crc6.o common/hermes_log.o \
	common/chan.o common/queue.o data_interfaces/tcp_interfaces.o data_interfaces/net.o

all: internal_deps utils
	$(MAKE) mercury
	$(MAKE) -C utils

mercury: $(MERCURY_LINK_INPUTS)
	$(CC) -o mercury  \
		$(MERCURY_LINK_INPUTS) $(LDFLAGS)

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
	rm -f mercury *.o third_party/chan/*.o
	$(MAKE) -C modem clean
	$(MAKE) -C datalink_arq clean
	$(MAKE) -C datalink_broadcast clean
	$(MAKE) -C data_interfaces clean
	$(MAKE) -C audioio clean
	$(MAKE) -C common clean
