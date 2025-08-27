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

CC = gcc
CFLAGS = -Wall -O2 -std=gnu11 -pthread -D_GNU_SOURCE -I../modem/freedv -I../modem -I../datalink_broadcast -I.. -I../audioio/ffaudio -I../common -I../data_interfaces

LDFLAGS=$(FFAUDIO_LINKFLAGS) -lm

all: arq.o fsm.o arith.o

arq.o: arq.c
	$(CC) $(CFLAGS) -c arq.c

fsm.o: fsm.c
	$(CC) $(CFLAGS) -c fsm.c

arith.o: arith.c
	$(CC) $(CFLAGS) -c arith.c

clean:
	rm -f *.o
