# Plan: Fix GPIF SM lifecycle in STARTFX3 / STOPFX3

## Problem

The GPIF state machine does not stop properly on STOPFX3, and cannot be
reliably restarted via STARTFX3.  This causes the device to wedge after
a stop/start cycle, making it unresponsive until USB re-enumeration.

### Root cause chain (original)

1. `rx888_stream` (or any host application) starts streaming, is killed
   mid-stream.
2. STOPFX3 — old code does `sleep(10)` + DMA reset but **never stops
   the GPIF state machine**.
3. GPIF SM continues pushing ADC data into DMA sockets that were just
   reset.  SM enters BUSY/WAIT (blocked on dead DMA producer socket).
4. STARTFX3 calls `CyU3PDmaMultiChannelReset()` — **blocks** because
   the GPIF still holds the DMA sockets.
5. USB setup callback never returns → host times out → device
   unresponsive to all EP0 → cascading failures.

---

## Implementation History

### Phase 0 — Test tooling (commit 0c670fb)

Added four `fx3_cmd` subcommands and `fw_test.sh` entries to detect the
broken stop/start behavior:

| Subcommand | What it tests |
|------------|---------------|
| `stop_gpif_state` | GPIF SM state ∈ {0,1} after STOPFX3 |
| `stop_start_cycle` | 5× STOP+START cycles with bulk reads |
| `pll_preflight` | STARTFX3 rejected when ADC clock is off |
| `wedge_recovery` | DMA backpressure → STOP+START recovery |

All four tests confirmed FAIL on the original firmware (baseline).

### Phase 1 — Fix STOPFX3 (commit 9d92e14)

Added `CyU3PGpifDisable(CyFalse)` to STOPFX3 (graceful stop, keep
waveform loaded).

**Problem discovered**: `CyFalse` (graceful mode) waits for the current
state to complete — can block if SM is stuck in BUSY/WAIT.

### Phase 2 — Fix STARTFX3 (commit 4924c66)

Changed to `CyU3PGpifDisable(CyTrue)` (force mode) in both STARTFX3
and STOPFX3.  Added `CyU3PGpifSMStart(0,0)` to STARTFX3.

**Problem discovered**: `CyU3PGpifDisable(CyTrue)` destroys the loaded
waveform descriptor.  A bare `CyU3PGpifSMStart(0,0)` after that
silently no-ops (returns SUCCESS but SM is in unloaded/invalid state).

### Phase 2b — Fix GpifLoad (commit d3f16d0)

To fix the unloaded-waveform problem:
- STARTFX3: changed `CyU3PGpifSMStart(0,0)` to `StartGPIF()` (which
  does `CyU3PGpifLoad` + `CyU3PGpifSMStart`).  Added `extern` for
  `StartGPIF()` and `CyFxGpifConfig`.
- STOPFX3: added `CyU3PGpifLoad(&CyFxGpifConfig)` after
  `CyU3PGpifDisable(CyTrue)`, intending to leave SM in RESET (state 0).

**Result: STOPFX3 still fails.**  Test output:
```
FAIL stop_gpif_state: GPIF state=2 after STOP (expected 0 or 1, SM still running)
```

---

## Current Status: Investigating GPIF state=2 after STOPFX3

### GPIF State Machine Map

From `SDDC_GPIF.h`, the 10 states:

| Index | Name | Role |
|-------|------|------|
| 0 | RESET | Start state |
| 1 | IDLE | Waiting for FW_TRG (SW input) |
| 2 | **TH0_RD** | **Thread 0 actively reading data** |
| 3 | TH1_RD_LD | Thread 1 read + reload counter |
| 4 | TH0_RD_LD | Thread 0 read + reload counter |
| 5 | TH0_BUSY | Thread 0 DMA full |
| 6 | TH1_RD | Thread 1 actively reading data |
| 7 | TH1_BUSY | Thread 1 DMA full |
| 8 | TH1_WAIT | Thread 1 wait for buffer |
| 9 | TH0_WAIT | Thread 0 wait for buffer |

State 2 = **TH0_RD**: the SM is actively reading ADC data into Thread 0.
This is not a stuck/BUSY state — the SM is running.

### Key State Machine Transitions (from gpif2model.xml)

```
RESET(0) ──[LOGIC_ONE]──► IDLE(1) ──[FW_TRG]──► TH0_RD_LD(4) ──[DMA_RDY_TH0]──► TH0_RD(2)
                                                                  ──[!DMA_RDY_TH0]──► TH0_BUSY(5)

TH0_RD(2) ──[DATA_CNT_HIT]──► TH1_RD_LD(3)    (buffer full, switch threads)
          (NO transition on !FW_TRG from this state!)

TH1_RD(6) ──[!FW_TRG]──► IDLE(1)              (only exit back to IDLE)
```

Critical: The **only** path from an active state back to IDLE is from
**TH1_RD (state 6)** via `!FW_TRG`.  State TH0_RD (2) has no FW_TRG
transition — it can only advance to TH1_RD_LD (3) via DATA_CNT_HIT.

### Hypothesis: Why state=2 persists after STOPFX3

The current STOPFX3 code (after commit d3f16d0):

```c
case STOPFX3:
    CyU3PUsbLPMEnable();
    CyU3PUsbGetEP0Data(wLength, glEp0Buffer, NULL);
    CyU3PGpifDisable(CyTrue);           // 1. Force-stop SM, destroy waveform
    CyU3PGpifLoad(&CyFxGpifConfig);     // 2. Reload waveform descriptor
    CyU3PDmaMultiChannelReset(...);      // 3. Reset DMA
    CyU3PUsbFlushEp(CY_FX_EP_CONSUMER); // 4. Flush EP
    isHandled = CyTrue;
    break;
```

**Three issues identified:**

**Issue 1 — `CyU3PGpifLoad()` re-enables the GPIF block.**
`CyFxGpifRegValue[0]` = `0x80008300` (the `CY_U3P_PIB_GPIF_CONFIG`
register), with bit 31 = ENABLE.  `CyU3PGpifLoad()` writes this to
hardware, re-enabling the GPIF.  With the SM loaded and enabled, it
begins executing from state 0 (RESET).

**Issue 2 — SW input (FW_TRG) is never deasserted.**
`CyU3PGpifControlSWInput(CyTrue)` is called in STARTFX3 (line 290) and
**never cleared** — there is no `CyU3PGpifControlSWInput(CyFalse)` call
anywhere in the codebase.  The original Phase 1 plan in
`PLAN-gpif-wedge-fix.md` explicitly included the deassert, but the
implementation dropped it.

**Issue 3 — GpifLoad runs before DMA reset.**
At step 2, the old DMA channel state still has buffers that may appear
"ready" to the freshly-loaded GPIF.

**Combined effect**: After GpifLoad re-enables the GPIF, the SM
auto-advances through the transition chain because FW_TRG is still
asserted and old DMA buffers appear ready:

```
RESET(0) ─[LOGIC_ONE]─► IDLE(1) ─[FW_TRG=true]─► TH0_RD_LD(4) ─[DMA_RDY_TH0?]─► TH0_RD(2)
```

This explains the observed GPIF state=2 after STOPFX3.

**Issue 4 — Unconditional SW input assert in STARTFX3.**
`CyU3PGpifControlSWInput(CyTrue)` at line 290 runs even if
`StartGPIF()` or DMA setup failed.

### Comparison with StopApplication (the working path)

`StopApplication()` in `StartStopApplication.c:143` uses:
```c
CyU3PGpifDisable(CyTrue);
CyU3PPibDeInit();            // tears down entire PIB block
```

It does **not** call `CyU3PGpifLoad()` — nothing re-enables the GPIF.
This is why the USB disconnect/reconnect path works correctly.

---

## Diagnostic Instrumentation (commit 3052681)

Added `CyU3PGpifGetSMState()` + `DebugPrint()` at each step of both
STARTFX3 and STOPFX3 handlers.  No functional changes — debug-only
instrumentation visible via `fx3_cmd debug` USB polling.

### Debug Message Legend

| Message | When | What it captures |
|---------|------|------------------|
| `GO in s=N` | STARTFX3 entry | SM state before any action |
| `GO dis s=N` | After GpifDisable in START | SM state after force-stop |
| `GO gp=R s=N` | After StartGPIF() | Return code R + SM state |
| `GO sw s=N` | After SWInput assert | SM state after FW_TRG set |
| `STP in s=N` | STOPFX3 entry | SM state before any action |
| `STP dis s=N` | After GpifDisable in STOP | SM state after force-stop |
| `STP ld s=N` | After GpifLoad in STOP | SM state after waveform reload |

### Test Procedure for Diagnostic Firmware

1. **Build**: `cd SDDC_FX3 && make clean && make`
2. **Flash** the resulting `SDDC_FX3.img`
3. **Regression check**: Run `fw_test.sh --firmware ../SDDC_FX3/SDDC_FX3.img`
   to confirm no regressions from the instrumentation (expected: all
   pre-existing tests pass, `stop_gpif_state` still fails as before).
4. **Capture diagnostic output**:
   - Terminal 1: `cd tests && ./fx3_cmd debug`
     (enables debug mode, polls USB debug buffer every 50ms)
   - Terminal 2: `cd tests && ./fx3_cmd stop_gpif_state`
5. **Capture Terminal 1 output**.  Expected pattern:
   ```
   GO in s=1        (SM was in IDLE or RESET before START)
   GO dis s=0       (GpifDisable stopped SM)
   GO gp=0 s=1      (StartGPIF loaded+started, SM in IDLE)
   GO sw s=2         (SWInput triggered IDLE→TH0_RD_LD→TH0_RD)
   ...
   STP in s=N        (SM was in active read state)
   STP dis s=0       (GpifDisable stopped SM — state should be 0 or undefined)
   STP ld s=2        (GpifLoad re-enabled GPIF — SM auto-advanced to TH0_RD!)
   ```
   The critical observation is whether `STP ld s=` shows a non-zero
   state, confirming that GpifLoad is the step that re-starts the SM.

### Buffer Size Note

The USB debug buffer is 100 bytes (`glBufDebug[100]`).  Each
`DebugPrint` message is ~20-25 bytes.  The `fx3_cmd debug` host polls
every 50ms, which should be fast enough to drain the buffer between
prints.  If messages are dropped, reduce to fewer instrumentation
points or switch to UART debug.

---

## Proposed Fix (pending diagnostic confirmation)

Once the instrumentation confirms the hypothesis, the fix has two parts:

### Fix A: Remove `CyU3PGpifLoad()` from STOPFX3, add SW input deassert

```c
case STOPFX3:
    CyU3PUsbLPMEnable();
    CyU3PUsbGetEP0Data(wLength, glEp0Buffer, NULL);
    CyU3PGpifControlSWInput(CyFalse);   // deassert FW_TRG first
    CyU3PGpifDisable(CyTrue);           // force-stop SM
    CyU3PDmaMultiChannelReset(&glMultiChHandleSlFifoPtoU);
    CyU3PUsbFlushEp(CY_FX_EP_CONSUMER);
    isHandled = CyTrue;
    break;
```

Rationale:
- Deassert SW input **before** disable so the SM doesn't see stale
  FW_TRG after any future reload.
- `CyU3PGpifDisable(CyTrue)` stops the SM.  No GpifLoad — nothing
  re-enables the GPIF.
- STARTFX3 already calls `StartGPIF()` which does Load+Start, so
  the waveform will be reloaded on the next START.

### Fix B: Make SW input assert conditional in STARTFX3

```c
    if (apiRetStatus == CY_U3P_SUCCESS)
    {
        apiRetStatus = StartGPIF();
        if (apiRetStatus == CY_U3P_SUCCESS)
        {
            CyU3PGpifControlSWInput(CyTrue);
            isHandled = CyTrue;
        }
    }
```

Move `CyU3PGpifControlSWInput(CyTrue)` inside the success path so
it only fires when StartGPIF() succeeded.

---

## Remaining Phases (from original plan)

### Phase 3: PLL Lock Pre-Flight Check in STARTFX3

Read Si5351 register 0, check bit 5 (PLL_A loss-of-lock).  Reject
STARTFX3 if PLL is unlocked.  See `PLAN-gpif-wedge-fix.md` Phase 3
for details.

### Phase 4: GPIF Watchdog in Main Loop

Auto-detect and recover from GPIF wedge in the ApplicationThread
100ms poll loop.  See `PLAN-gpif-wedge-fix.md` Phase 4 for details.

---

## Files Modified

| File | Phase | Change |
|------|-------|--------|
| `tests/fx3_cmd.c` | 0 | 4 test subcommands |
| `tests/fw_test.sh` | 0 | 4 test entries |
| `SDDC_FX3/USBHandler.c` | 1,2,2b,diag | STOPFX3/STARTFX3 fixes + instrumentation |
| `SDDC_FX3/StartStopApplication.c` | 2b | `StartGPIF()` made extern-accessible |

## What Is NOT Changed

- **GPIF state machine (SDDC_GPIF.h)** — no SM redesign needed.
- **StopApplication / StartApplication** — the USB connect/disconnect
  path is already correct.
- **DMA channel configuration** — we reset and re-arm, not
  destroy/recreate.
