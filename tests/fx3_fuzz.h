/*
 * fx3_fuzz.h — seeded USB/vendor-command fuzzers for the SDDC_FX3 firmware
 * (issue #139).
 *
 *   protocol_fuzz — seeded EP0 control-transfer fuzzer (bmRequestType,
 *                   bRequest, wValue, wIndex, wLength, payload).
 *   stream_fuzz   — seeded bulk + host-lifecycle fuzzer (async reads of
 *                   random size/timeout, cancellation, EP0 ops while reads
 *                   pending, release/re-claim + close/reopen, abandon).
 *
 * Both treat the firmware as a black box: individual STALLs are expected and
 * only logged; the PASS/FAIL gate is a periodic health probe (TESTFX3 +
 * GETSTATS).  On failure a seeded operation ring buffer is dumped so the run
 * reproduces with the same seed.
 */
#pragma once

#include <stdint.h>
#include <libusb-1.0/libusb.h>

/* Standalone subcommands.  Take **handle so a reset-induced re-enumeration
 * can propagate a fresh handle back to main()'s cleanup path.  num_ops/seconds
 * <= 0 fall back to defaults; seed == 0 picks a time-based seed (printed). */
int fuzz_protocol(libusb_device_handle **h_inout, long num_ops, uint64_t seed);
int fuzz_stream(libusb_device_handle **h_inout, double seconds, uint64_t seed);

/* #142 isolation: well-formed vendor requests with only the direction bit
 * flipped — confirms wrong-direction alone wedges EP0 (build-free falsifier). */
int fuzz_dir_mismatch(libusb_device_handle **h_inout, long num_ops, uint64_t seed);

/* #149: deterministic exhaustive sweep of every bRequest 0..255 x {IN,OUT};
 * asserts no wrong-direction/unknown request is accepted and the device
 * survives — the regression test for the #142 direction guard. */
int fuzz_ep0_sweep(libusb_device_handle **h_inout);

/* #148: the standalone fuzzers return 2 when the device is EP0-healthy but no
 * longer streaming (Si5351 likely reconfigured by fuzz); main() maps that to a
 * reload-and-re-verify recovery check. */

/* Bounded soak bursts — seed derived from the soak's rand() so they stay
 * reproducible from the soak seed.  Obey soak conventions (STOPFX3 on exit,
 * no intentional re-enumeration). */
int do_test_protocol_fuzz_burst(libusb_device_handle *h);
int do_test_stream_fuzz_burst(libusb_device_handle *h);
