# Makefile for UDP JSON demo

CFLAGS  += -Wno-stringop-truncation -DTEST_MAIN
LDFLAGS = -pthread

# Objects
OBJS = ui_communication.o

# Target
TARGET = ui_communication

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

ui_communication.o: ui_communication.c ui_communication.h
	$(CC) $(CFLAGS) -c ui_communication.c

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
