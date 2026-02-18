## STARTFX3 intermittent I/O Error during soak test (~3% failure rate)

### Summary

During extended soak testing, STARTFX3 vendor requests intermittently fail
with Input/Output Error (LIBUSB_ERROR_IO). The failure rate is approximately
2-3% of cycles. The device self-recovers after ~2 seconds, but the failures
are spurious -- they do not indicate an actual hardware or streaming fault.

### Observed behaviour

Every failure follows the same pattern:

1. A scenario calls STARTADC (succeeds), then STARTFX3 (fails with I/O Error).
2. cmd_u32_retry retries STARTFX3 three times with 500ms / 1s backoff -- all attempts fail.
3. The inter-scenario health check (TESTFX3) also fails on the first attempt.
4. After a 2-second recovery window, TESTFX3 passes and the next scenario succeeds.

Example from a 252-cycle run (seed 1771373722):

    PASS freq_hop: data flowed at all 5 frequencies
    FAIL stop_start_cycle: STARTFX3 on cycle 1: Input/Output Error
    HEALTH FAIL: TESTFX3 failed: Input/Output Error
    PASS debug_race: device survived 50 rapid debug poll cycles (issue #8)

7 failures out of 252 cycles (2.78%). All are STARTFX3 I/O errors.
Affected scenarios: stop_start_cycle, wedge_recovery, abandoned_stream,
clock_pull, double_stop -- i.e. any scenario that calls STARTFX3.

### Root cause

**Primary: STARTADC blocks the USB callback thread for 1 second.**

The STARTADC handler in USBHandler.c calls CyU3PThreadSleep(1000) after
programming the Si5351 (line 217). This blocks the USB driver thread -- the
same thread that dispatches all EP0 vendor-request callbacks.

When the host sends STARTFX3 immediately after STARTADC completes (from the
host's perspective, STARTADC finishes as soon as CyU3PUsbGetEP0Data ACKs the
data phase), the STARTFX3 SETUP+DATA tokens are received by the FX3 USB
hardware and buffered, but the callback cannot be dispatched until the
STARTADC callback's 1-second sleep finishes. STARTFX3's STATUS IN phase is
therefore delayed by up to 1 second.

The host-side control-transfer timeout is CTRL_TIMEOUT_MS = 1000 (1 second).
The STARTFX3 STATUS IN arrives right at this boundary. Normal scheduling
jitter (USB bus latency, RTOS tick resolution, host-side thread scheduling)
pushes ~3% of transfers past the deadline, producing LIBUSB_ERROR_IO.

**Secondary: STARTFX3 error path leaves isHandled = CyFalse.**

When CyU3PDmaMultiChannelSetXfer or StartGPIF() fails inside the STARTFX3
handler (USBHandler.c lines 314-322), isHandled is never set to CyTrue. The
SDK's default handler for unhandled vendor requests STALLs EP0, but without
an explicit CyU3PUsbStall() call the STALL may not be a clean protocol STALL.
This leaves EP0 unresponsive to subsequent control transfers -- explaining why
TESTFX3 also fails in the health check immediately after.

Compare with the preflight-check error path (lines 305-310) which correctly
calls CyU3PUsbStall(0, CyTrue, CyFalse) and sets isHandled = CyTrue.

### Proposed fix

**1. STARTADC: replace 1s blocking sleep with PLL lock poll (USBHandler.c)**

Replace CyU3PThreadSleep(1000) with a poll loop that checks
si5351_pll_locked() at 1ms intervals, up to 100ms. Si5351 PLL lock time is
typically 1-10ms; the 100ms ceiling provides ample margin. This reduces
USB-thread blocking from 1000ms to ~10-50ms, leaving >900ms of headroom
for STARTFX3's 1-second host timeout.

**2. STARTFX3: handle DMA/GPIF setup failure explicitly (USBHandler.c)**

After the DMA/GPIF bring-up attempt, always set isHandled = CyTrue (the data
phase was already consumed by CyU3PUsbGetEP0Data). On failure, call
CyU3PUsbStall(0, CyTrue, CyFalse) so the host receives a clean protocol
STALL that clears on the next SETUP token -- matching the preflight-check
error path.

**3. STARTFX3: reset glDMACount before GPIF start (USBHandler.c)**

Set glDMACount = 0 before starting the GPIF state machine. The watchdog's
stall detector skips when curDMA == 0, giving the new streaming session a
clean grace period. This prevents a race where the watchdog sees stale
glDMACount == prevDMACount from the previous session and attempts recovery
concurrently with STARTFX3's DMA/GPIF bring-up.

**4. Increase CTRL_TIMEOUT_MS to 2000 (tests/fx3_cmd.c)**

Even after fix (1), a 2-second host timeout provides a comfortable safety
margin against future firmware changes, USB bus congestion, or unusual Si5351
lock times. This only affects the test harness, not firmware.

### Reproduction

Build firmware and test harness, then run:

    ./tests/soak_test.sh --firmware SDDC_FX3/SDDC_FX3.img --hours 1 --seed 1771373722

With seed 1771373722, failures appear around cycles 126, 166, 196, 201, 215,
247, and 249.

### Key code references

| Location | Relevance |
|----------|-----------|
| USBHandler.c:217 | CyU3PThreadSleep(1000) in STARTADC -- the blocking sleep |
| USBHandler.c:294-326 | STARTFX3 handler -- error path missing isHandled = CyTrue |
| USBHandler.c:305-310 | Preflight-check error path -- correct STALL + isHandled pattern |
| RunApplication.c:208-281 | Watchdog recovery -- races with STARTFX3 on DMA/GPIF state |
| tests/fx3_cmd.c:63 | CTRL_TIMEOUT_MS 1000 -- host-side timeout definition |
| tests/fx3_cmd.c:148-159 | cmd_u32_retry -- retry logic for transient EP0 failures |
| tests/fx3_cmd.c:2218-2244 | Soak inter-scenario recovery -- STOPFX3 + health check |
| driver/Si5351.c:152-163 | si5351_pll_locked() -- PLL lock register read (for poll loop) |
