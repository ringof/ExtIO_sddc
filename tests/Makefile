CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
LDLIBS  := $(shell pkg-config --libs libusb-1.0)
CFLAGS  += $(shell pkg-config --cflags libusb-1.0)

PROGS = fx3_cmd

all: $(PROGS)

fx3_cmd: fx3_cmd.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(PROGS)

.PHONY: all clean
