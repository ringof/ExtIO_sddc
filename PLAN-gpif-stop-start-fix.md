# Plan: Fix GPIF SM lifecycle in STARTFX3 / STOPFX3

## Problem

Tests 25-29 and 31-33 fail because STARTFX3 blocks the USB callback
thread, making the device unresponsive.

### Root cause chain

1. `rx888_stream` (test 1) starts streaming, is killed mid-stream.
2. Test 9 sends STOPFX3 — old code does `sleep(10)` + DMA reset but
   **never stops the GPIF state machine**.
3. GPIF SM continues pushing ADC data into DMA sockets that were just
   reset.  SM enters BUSY/WAIT (blocked on dead DMA producer socket).
4. Test 25's STARTFX3 calls `CyU3PDmaMultiChannelReset()` — **blocks**
   because the GPIF still holds the DMA sockets.
5. USB setup callback never returns → host times out → device
   unresponsive to all EP0 → tests 26-33 cascade.

### Why Phase 1 alone didn't fix it

Phase 1 added `CyU3PGpifDisable(CyFalse)` to STOPFX3, but:

- Used graceful mode (`CyFalse`) which waits for the current state to
  complete — can block if SM is stuck in BUSY/WAIT.
- STARTFX3 was not updated to restart the SM after it was disabled.
  After `GpifDisable`, `CyU3PGpifSMStart(0,0)` is required.

## Fix (two changes in `USBHandler.c`)

### Change 1: STOPFX3 — force-stop before DMA reset

```c
case STOPFX3:
    CyU3PUsbLPMEnable();
    CyU3PUsbGetEP0Data(wLength, glEp0Buffer, NULL);
    CyU3PGpifDisable(CyTrue);   /* force-stop SM immediately */
    CyU3PDmaMultiChannelReset (&glMultiChHandleSlFifoPtoU);
    CyU3PUsbFlushEp(CY_FX_EP_CONSUMER);
    isHandled = CyTrue;
    break;
```

Rationale:
- `CyTrue` = force mode, terminates immediately even if stuck in
  BUSY/WAIT.  Matches `StopApplication()` which also uses `CyTrue`.
- Removes the now-unnecessary `CyU3PGpifControlSWInput(CyFalse)` —
  force-disable doesn't need it.

### Change 2: STARTFX3 — force-stop + DMA reset + SM restart

```c
case STARTFX3:
    CyU3PUsbLPMDisable();
    CyU3PUsbGetEP0Data(wLength, glEp0Buffer, NULL);
    CyU3PGpifDisable(CyTrue);   /* release DMA sockets if SM stuck */
    CyU3PDmaMultiChannelReset (&glMultiChHandleSlFifoPtoU);
    apiRetStatus = CyU3PDmaMultiChannelSetXfer (
        &glMultiChHandleSlFifoPtoU, FIFO_DMA_RX_SIZE, 0);
    if (apiRetStatus == CY_U3P_SUCCESS)
    {
        apiRetStatus = CyU3PGpifSMStart(0, 0);
        if (apiRetStatus == CY_U3P_SUCCESS)
            isHandled = CyTrue;
    }
    CyU3PGpifControlSWInput(CyTrue);
    break;
```

Rationale:
- `CyU3PGpifDisable(CyTrue)` is idempotent — safe to call even if SM
  is already stopped (returns error which we ignore).
- Waveform memory survives `GpifDisable`, so no `CyU3PGpifLoad`
  needed — just `CyU3PGpifSMStart(0,0)` to restart from RESET state.
- `CyU3PGpifControlSWInput(CyTrue)` triggers the RESET → IDLE →
  TH0_RD transition to begin data flow.
- Removes the old pattern of toggling SW input without stopping the
  SM, which was the core failure mode.

## Expected test impact

| Test | Before | After | Notes |
|------|--------|-------|-------|
| 25 stop_gpif_state    | FAIL | PASS | SM properly stopped, STARTFX3 no longer blocks |
| 26 stop_start_cycle   | FAIL | PASS | Cascading fix — STOP+START cycle works |
| 28 wedge_recovery     | FAIL | PASS | Force-stop recovers from DMA backpressure |
| 27 pll_preflight      | FAIL | FAIL | Needs separate Phase 3 (PLL check in STARTFX3) |
| 29 pib_overflow       | FAIL | PASS | Device responsive after prior tests pass |
| 31-33 stream          | FAIL | PASS | Device in good state for final streaming test |

## Files changed

- `SDDC_FX3/USBHandler.c` — STARTFX3 and STOPFX3 handlers
