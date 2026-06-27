# tests/Makefile — Build test tools for SDDC_FX3 firmware validation
#
# Targets:
#   all            Build fx3_cmd and rx888_stream (default)
#   fx3_cmd        Vendor command exerciser
#   rx888_stream   USB streamer (built from rx888_tools submodule)
#   check-health   Build + run the health_evaluate() host unit test
#                  (no FX3 SDK / hardware / libusb needed)
#   clean          Remove build artifacts
#
# Prerequisites:
#   sudo apt install libusb-1.0-0-dev
#   git submodule update --init   (if rx888_tools/ is empty)

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter

# Keep libusb discovery lazy and scoped to fx3_cmd/rx888_stream builds so
# host-only targets such as check-health remain usable without libusb-dev.
LIBUSB_CFLAGS = $(shell pkg-config --cflags libusb-1.0)
LIBUSB_LDLIBS = $(shell pkg-config --libs libusb-1.0)
FX3_CFLAGS    = $(CFLAGS) $(LIBUSB_CFLAGS)

# rx888_tools submodule paths
RX888_TOOLS := rx888_tools
RX888_STREAM_BIN := $(RX888_TOOLS)/rx888_stream

all: fx3_cmd rx888_stream

# fx3_cmd is built from several modules (issue #139 modularization start).
FX3_OBJS := fx3_cmd.o fx3_usb.o fx3_stats.o fx3_bulk.o fx3_fuzz.o fx3_lifecycle.o
FX3_HDRS := fx3_proto.h fx3_usb.h fx3_stats.h fx3_bulk.h fx3_fuzz.h fx3_lifecycle.h

fx3_cmd: $(FX3_OBJS)
	$(CC) $(FX3_CFLAGS) -o $@ $(FX3_OBJS) $(LIBUSB_LDLIBS) $(LDLIBS)

%.o: %.c $(FX3_HDRS)
	$(CC) $(FX3_CFLAGS) -c -o $@ $<

# Standalone host-side unit test for the Si5351 synthesis math (issue #133).
# No libusb/hardware dependency; only needs libm.
si5351_math_test: si5351_math_test.c
	$(CC) $(CFLAGS) -o $@ $< -lm

.PHONY: check-math
check-math: si5351_math_test
	./si5351_math_test

# Monte-Carlo error characterization + gnuplot error plots (issue #133).
si5351_mc: si5351_mc.c
	$(CC) $(CFLAGS) -o $@ $< -lm

.PHONY: mc-plot
mc-plot: si5351_mc
	./si5351_mc
	gnuplot si5351_mc.gp
	@echo "wrote si5351_mc_scatter.png si5351_mc_cdf.png"

# Divider-search cost benchmark (issue #133).
si5351_bench: si5351_bench.c
	$(CC) $(CFLAGS) -o $@ $< -lm

.PHONY: bench
bench: si5351_bench
	./si5351_bench

rx888_stream: $(RX888_STREAM_BIN)
	@# Symlink into tests/ so fw_test.sh finds it alongside fx3_cmd
	ln -sf $(RX888_STREAM_BIN) $@

$(RX888_STREAM_BIN): $(RX888_TOOLS)/src/rx888_stream.c $(RX888_TOOLS)/src/ezusb.c
	$(MAKE) -C $(RX888_TOOLS) rx888_stream

# --- Host unit tests (no FX3 SDK / hardware / libusb) --------------------
HOST_HEALTH_BIN  := host/health_eval_test
HOST_HEALTH_DEPS := host/health_eval_test.c host/health_host_shim.h \
                    host/cyu3types.h ../SDDC_FX3/health.c ../SDDC_FX3/health.h \
                    ../SDDC_FX3/protocol.h ../SDDC_FX3/gpif_states.h

$(HOST_HEALTH_BIN): $(HOST_HEALTH_DEPS)
	$(CC) -std=c11 -Wall -Wextra -DHEALTH_HOST_TEST -Ihost -I../SDDC_FX3 -o $@ host/health_eval_test.c

check-health: $(HOST_HEALTH_BIN)
	./$(HOST_HEALTH_BIN)

clean:
	rm -f fx3_cmd $(FX3_OBJS) rx888_stream si5351_math_test si5351_mc si5351_bench $(HOST_HEALTH_BIN)
	$(MAKE) -C $(RX888_TOOLS) clean 2>/dev/null || true

.PHONY: all clean rx888_stream check-health
