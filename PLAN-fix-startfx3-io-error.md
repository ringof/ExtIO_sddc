# Plan: Fix STARTFX3 Input/Output Error during soak test

## Problem

Soak test shows ~3% failure rate — all failures are `STARTFX3: Input/Output Error`.
The device recovers after 2s, but the failure shouldn't happen at all.

## Root Cause

### Primary: STARTADC blocks the USB thread for 1 second

`USBHandler.c:217` — the STARTADC handler calls `CyU3PThreadSleep(1000)` to
wait for the Si5351 PLL to lock.  This blocks the USB callback thread for the
full duration.  When the host sends STARTFX3 immediately after STARTADC, the
STARTFX3 callback is queued behind the 1-second sleep.  The STARTFX3 STATUS IN
phase is delayed by ~1s, right at the host's `CTRL_TIMEOUT_MS = 1000` boundary.
Timing jitter causes ~3% of STARTFX3 transfers to exceed the deadline.

### Secondary: STARTFX3 error path leaves `isHandled = CyFalse`

`USBHandler.c:314-322` — when `CyU3PDmaMultiChannelSetXfer` or `StartGPIF()`
fails, `isHandled` is never set to `CyTrue`.  The SDK's default handler STALLs
EP0, but the STALL is not a proper protocol STALL (no explicit
`CyU3PUsbStall()` call), which can leave EP0 unresponsive to subsequent
control transfers (explaining why TESTFX3 also fails in the health check).

Compare with the preflight-check path (line 305–310) which correctly calls:
```c
CyU3PUsbStall(0, CyTrue, CyFalse);
isHandled = CyTrue;
```

## Proposed Changes

### Change 1 — STARTADC: replace 1s blocking sleep with PLL lock poll

**File:** `SDDC_FX3/USBHandler.c`, STARTADC handler (~line 217)

Replace:
```c
CyU3PThreadSleep(1000);
```

With a poll loop that checks PLL lock status:
```c
/* Poll PLL lock — typically <10 ms for Si5351.
 * Max 100 iterations × 1 ms = 100 ms worst-case.
 * This keeps the USB thread unblocked for STARTFX3
 * arriving shortly after STARTADC. */
{
    int i;
    for (i = 0; i < 100; i++) {
        CyU3PThreadSleep(1);
        if (si5351_pll_locked()) break;
    }
}
```

**Rationale:** Si5351 PLL lock time is typically 1–10 ms.  The previous 1000 ms
sleep was a conservative blanket wait.  A polled approach returns as soon as
the PLL is locked, reducing USB thread blocking from 1 s to ~10–50 ms.  The
100 ms ceiling provides a safety margin for unusual Si5351 configurations.

### Change 2 — STARTFX3: handle DMA/GPIF setup failure explicitly

**File:** `SDDC_FX3/USBHandler.c`, STARTFX3 handler (~lines 314–323)

After the existing `if (apiRetStatus == CY_U3P_SUCCESS)` blocks, add an
explicit error path:

```c
apiRetStatus = CyU3PDmaMultiChannelSetXfer(...);
if (apiRetStatus == CY_U3P_SUCCESS) {
    apiRetStatus = StartGPIF();
    if (apiRetStatus == CY_U3P_SUCCESS) {
        CyU3PGpifControlSWInput(CyTrue);
    }
}
if (apiRetStatus != CY_U3P_SUCCESS) {
    /* DMA or GPIF setup failed — report to host and
     * ensure EP0 is left in a clean state. */
    DebugPrint(4, "\r\nSTARTFX3 fail: %d", apiRetStatus);
    CyU3PUsbStall(0, CyTrue, CyFalse);
}
isHandled = CyTrue;   /* always — GetEP0Data already consumed data phase */
```

**Rationale:** `isHandled = CyTrue` must always be set after `GetEP0Data` has
consumed the data phase.  The explicit `CyU3PUsbStall` sends a clean protocol
STALL that the host/HCD can recover from on the next SETUP packet.

### Change 3 — STARTFX3: reset DMA count to prevent watchdog false-positive

**File:** `SDDC_FX3/USBHandler.c`, STARTFX3 handler (before StartGPIF)

Add:
```c
glDMACount = 0;
```

**Rationale:** Between scenarios, `glDMACount` retains the stale value from the
previous streaming session.  The watchdog's stall detector checks
`curDMA == prevDMACount && curDMA > 0`.  Resetting to zero gives the GPIF
startup a clean grace period — the watchdog skips stall detection when
`curDMA == 0`.  This prevents the watchdog from racing with STARTFX3's
DMA/GPIF bring-up.

### ~~Change 4~~ — Dropped

CTRL_TIMEOUT_MS stays at 1000 ms.  Changes 1–3 reduce STARTADC blocking
from 1 s to ~10–50 ms, which provides sufficient margin within the existing
1 s host timeout.

## Validation

After applying these changes:

1. **Build firmware** and flash to device.
2. **Run soak test** for 1+ hours:
   ```
   ./soak_test.sh --firmware ../SDDC_FX3/SDDC_FX3.img --hours 1
   ```
3. **Expected result:** Zero `STARTFX3: Input/Output Error` failures.
   The previous ~3% failure rate should drop to 0%.
4. **Check STARTADC timing:** The PLL lock poll should complete in <50 ms
   (visible in firmware debug output if UART connected).

## Regression

- STARTADC: still blocks until PLL is locked; poll replaces fixed sleep
  but final state is identical (PLL locked, clock stable).
- STARTFX3: success path unchanged; only the error path gets explicit
  STALL + isHandled.
- Watchdog: `glDMACount = 0` in STARTFX3 is equivalent to what
  `StartApplication()` already does at USB enumeration time.
- CTRL_TIMEOUT_MS: unchanged (1000 ms), no regression possible.
