# tests/Makefile — Build test tools for SDDC_FX3 firmware validation
#
# Targets:
#   all            Build fx3_cmd and rx888_stream (default)
#   fx3_cmd        Vendor command exerciser
#   rx888_stream   USB streamer (built from rx888_tools submodule)
#   clean          Remove build artifacts
#
# Prerequisites:
#   sudo apt install libusb-1.0-0-dev
#   git submodule update --init   (if rx888_tools/ is empty)

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
LDLIBS  := $(shell pkg-config --libs libusb-1.0)
CFLAGS  += $(shell pkg-config --cflags libusb-1.0)

# rx888_tools submodule paths
RX888_TOOLS := rx888_tools
RX888_STREAM_BIN := $(RX888_TOOLS)/rx888_stream

all: fx3_cmd rx888_stream

fx3_cmd: fx3_cmd.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

rx888_stream: $(RX888_STREAM_BIN)
	@# Symlink into tests/ so fw_test.sh finds it alongside fx3_cmd
	ln -sf $(RX888_STREAM_BIN) $@

$(RX888_STREAM_BIN): $(RX888_TOOLS)/src/rx888_stream.c $(RX888_TOOLS)/src/ezusb.c
	$(MAKE) -C $(RX888_TOOLS) rx888_stream

clean:
	rm -f fx3_cmd rx888_stream
	$(MAKE) -C $(RX888_TOOLS) clean 2>/dev/null || true

.PHONY: all clean rx888_stream
