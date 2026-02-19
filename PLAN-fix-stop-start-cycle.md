# Plan: Fix stop_start_cycle failure at cycle 2

## Problem

`stop_start_cycle` consistently fails at cycle 2/5 with 0 bytes received:

```
FAIL stop_start_cycle: cycle 2/5: only 0 bytes (expected >= 1024, stream not flowing)
#   libusb_rc=-1 (Input/Output Error), GPIF_state=9, DMA_count=8, PIB_err=4, faults=1
```

Cycle 1 passes (via `primed_start_and_read_retry`). Cycle 2 always fails
(via `primed_start_and_read`). The firmware diagnostics show data IS being
produced (DMA_count=7-8) but the host receives nothing (0 bytes, IO Error).

## Root Cause

`primed_start_and_read` calls `libusb_clear_halt(EP1_IN)` on **every**
invocation (line 1441). This sends `CLEAR_FEATURE(ENDPOINT_HALT)` to the FX3.
The firmware's CLEAR_FEATURE handler (USBHandler.c:169-173) runs:

```c
CyU3PUsbStall (wIndex, CyFalse, CyTrue);   // clear stall + reset toggle
CyU3PUsbFlushEp (CY_FX_EP_CONSUMER);        // flush FIFO state
```

The comment at line 157 explicitly warns:

> on a non-stalled endpoint it has side-effects that corrupt internal
> USB controller state across repeated open/close cycles

After cycle 1's successful data flow + STOPFX3, the endpoint is **not
stalled** — it was cleanly stopped and flushed. The `clear_halt` at the
start of cycle 2 calls `CyU3PUsbStall(CyFalse, CyTrue)` on this
non-stalled endpoint, corrupting the USB controller's internal state
(likely the ERDY generation mechanism for USB 3.0 flow control).

When STARTFX3 restarts GPIF and DMA produces data (DMA_count=8), the USB
controller never signals ERDY to the host. Data accumulates in the FX3's
USB endpoint buffers, the host's bulk TD gets no response, and eventually
the xHCI reports TRANSFER_ERROR → `LIBUSB_ERROR_IO`.

**Why cycle 1 works:** The `primed_start_and_read_retry` variant retries up
to 3 times. If the first attempt fails, the 500ms sleep allows the watchdog
to fire, and the retry's STARTFX3 starts with a fresh `CyU3PGpifDisable` +
full pipeline rebuild. The endpoint may also enter a proper halted state
after the error, making the retry's `clear_halt` operate correctly (on a
halted endpoint).

**Why the retry doesn't help cycle 2:** `stop_start_cycle` only uses the
retry variant for `i == 0`. Cycle 2 uses the non-retry variant which gets
one shot — and that shot is poisoned by the unnecessary `clear_halt`.

## Fix (all in `tests/fx3_cmd.c`)

### Change 1: Remove `libusb_clear_halt` from `primed_start_and_read`

The `clear_halt` at device open (line 210) handles the initial xHCI endpoint
reset. Between clean stop/start cycles, no `clear_halt` is needed — the xHCI
endpoint is in Running state and can accept new TDs without reset.

**Before (line 1439-1441):**
```c
    /* 1. Clear halt — reset xHCI endpoint ring so stale error state
     *    from a prior test doesn't block the new transfer. */
    libusb_clear_halt(h, EP1_IN);
```

**After:**
```c
    /* Note: do NOT call libusb_clear_halt here.  Between clean stop/start
     * cycles the endpoint is not halted, and clear_halt on a non-stalled
     * endpoint triggers CyU3PUsbStall(CyFalse, CyTrue) in the firmware's
     * CLEAR_FEATURE handler, which corrupts USB controller ERDY state
     * after data has flowed.  The clear_halt at device open (open_rx888)
     * handles the initial xHCI endpoint reset; error recovery is handled
     * by the retry variant below. */
```

### Change 2: Add `libusb_clear_halt` to the retry path

After a failed attempt, the endpoint may be in xHCI Halted state (from
TRANSFER_ERROR). The retry needs `clear_halt` to recover. Also send STOPFX3
to ensure the GPIF is stopped from the failed attempt.

**Before (lines 1493-1504):**
```c
    int r = primed_start_and_read(h, len, timeout_ms);
    if (r >= 0 || (r != LIBUSB_ERROR_TIMEOUT && r != LIBUSB_ERROR_IO))
        return r;
    /* First retry — the STARTFX3 inside failed; GPIF never started,
     * so no STOPFX3 needed before retrying. */
    usleep(500000);
    r = primed_start_and_read(h, len, timeout_ms);
    if (r >= 0 || (r != LIBUSB_ERROR_TIMEOUT && r != LIBUSB_ERROR_IO))
        return r;
    /* Second retry */
    usleep(1000000);
    return primed_start_and_read(h, len, timeout_ms);
```

**After:**
```c
    int r = primed_start_and_read(h, len, timeout_ms);
    if (r >= 0 || (r != LIBUSB_ERROR_TIMEOUT && r != LIBUSB_ERROR_IO))
        return r;
    /* First retry — previous attempt may have started GPIF (STARTFX3
     * succeeded but bulk read failed).  Stop streaming, clear the
     * xHCI endpoint error state, then retry. */
    cmd_u32(h, STOPFX3, 0);
    usleep(500000);
    libusb_clear_halt(h, EP1_IN);
    r = primed_start_and_read(h, len, timeout_ms);
    if (r >= 0 || (r != LIBUSB_ERROR_TIMEOUT && r != LIBUSB_ERROR_IO))
        return r;
    /* Second retry */
    cmd_u32(h, STOPFX3, 0);
    usleep(1000000);
    libusb_clear_halt(h, EP1_IN);
    return primed_start_and_read(h, len, timeout_ms);
```

### Change 3: Update `primed_start_and_read` header comment

Update the function's doc comment (lines 1395-1412) to remove the
`libusb_clear_halt` from the documented sequence and explain why it's
omitted.

## What this does NOT change

- **Firmware**: No FX3 code changes. The CLEAR_FEATURE handler remains as-is.
- **Device open**: `libusb_clear_halt` at line 210 remains — it's needed for
  the initial xHCI endpoint restart after the previous process closed the fd.
- **`primed_start_and_read` async approach**: Still submits the bulk TD before
  STARTFX3 to avoid the PIB overflow race.
- **`stop_start_cycle` test structure**: Still uses retry for cycle 1, non-retry
  for cycles 2+.

## Build

Same as before: `cd tests && make` (or `gcc -O2 -Wall -o fx3_cmd fx3_cmd.c -lusb-1.0`).

## Validation

- **Primary:** Run `./fx3_cmd stop_start_cycle` — all 5 cycles should pass.
- **Regression:** Run `./fx3_cmd stop`, `./fx3_cmd adc 32000000`,
  `./fx3_cmd stop_start_cycle` in sequence (matches test pattern #1).
- **Retry still works:** Run after a device power cycle or after a test that
  leaves the endpoint in error state — cycle 1's retry should still recover.

## Regression

- `hw_smoke` uses `primed_start_and_read_retry` — the retry path still has
  `clear_halt`, so it continues to work.
- `stop_gpif_state` doesn't use primed reads — unaffected.
- `pll_preflight` doesn't use primed reads — unaffected.
- Other streaming tests use `rx888_stream` (separate process) — unaffected.
