# HERMES Modem Test Utilities Makefile
#
# Copyright (C) 2025 Rhizomatica
# Author: Rafael Diniz <rafael@riseup.net>

CC = gcc
CFLAGS = -Wall -O2 -std=gnu11 -pthread -D_GNU_SOURCE -I.. -I../modem -I../modem/freedv -I../common -I../datalink_arq -I../data_interfaces -I../audioio/ffaudio

LDFLAGS = -lm

all: arq_test_client loopback_test broadcast_test

arq_test_client: arq_test_client.c
	$(CC) $(CFLAGS) -o arq_test_client arq_test_client.c $(LDFLAGS)

loopback_test: loopback_test.c
	$(CC) $(CFLAGS) -o loopback_test loopback_test.c $(LDFLAGS)

broadcast_test: broadcast_test.c
	$(CC) $(CFLAGS) -o broadcast_test broadcast_test.c ../datalink_broadcast/kiss.o $(LDFLAGS)

clean:
	rm -f arq_test_client loopback_test

.PHONY: all clean
