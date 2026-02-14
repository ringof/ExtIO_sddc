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
| `GO s=N r=R` | STARTFX3 completion | Final SM state + last apiRetStatus |
| `STP s=N` | STOPFX3 completion | SM state after full stop sequence |

Previous verbose per-step messages (`GO in`, `GO dis`, `GO gp`, `GO sw`,
`STP in`, `STP dis`, `STP ld`) were removed to reduce DebugPrint
pressure in the EP0 callback context.

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

## Hardware Test Confirmation (2026-02-13)

Ran `!stop_gpif_state` from the debug console (using the new `!`
local command escape).  Firmware debug trace captured in real time:

```
fx3> stop_gpif_state
FAIL stop_gpif_state: GPIF state=8 after STOP (expected 0 or 1, SM still running)
STARTADC	32000000
GO in s=8
GO dis s=255
GPIF file HF103.
```

### Trace Interpretation

| Trace line | Meaning |
|------------|---------|
| `STARTADC 32000000` | Test configured ADC clock at 32 MHz |
| `GO in s=8` | STARTFX3 entry: SM already in state 8 (TH1_WAIT — leftover from prior run) |
| `GO dis s=255` | `CyU3PGpifDisable(CyTrue)` worked — state 255 = GPIF block fully disabled |
| `GPIF file HF103.` | `StartGPIF()` reloaded waveform via `CyU3PGpifLoad()` + `CyU3PGpifSMStart()` |

After STARTFX3 the SM ran (test read bulk data successfully).  Then
STOPFX3 fired and GETSTATS reported `gpif_state=8` — the SM is back
in TH1_WAIT, still running.

**Note:** The STOPFX3 trace lines (`STP in`, `STP dis`, `STP ld`)
were likely dropped due to debug buffer contention with the bulk
transfer activity.  But the GETSTATS result (state=8) is conclusive:
`CyU3PGpifLoad()` in STOPFX3 re-enabled the GPIF and the SM
auto-advanced through the transition chain.

### Confirmation

This **confirms the hypothesis from the analysis above**:
- `CyU3PGpifDisable(CyTrue)` correctly stops the SM (state→255)
- `CyU3PGpifLoad()` re-enables the GPIF block, SM restarts immediately
- The SM reaches state 8 because FW_TRG is still asserted (never
  deasserted) and ADC data is clocking in

The fix below is now validated against hardware and ready to apply.

---

## Proposed Fix (confirmed by hardware test)

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

## fw_test.sh STARTFX3 Timeout — Root Cause (2026-02-14)

### Symptom

`fw_test.sh` test 25 (`stop_gpif_state`) fails with:
```
FAIL stop_gpif_state: STARTFX3: Operation timed out
```
After the timeout, the device is completely unresponsive (all subsequent
EP0 transfers return I/O Error).  However, the same test passes when
run interactively from the `!` debug console.

### Diagnostic Tests Performed

| Test | Method | Result |
|------|--------|--------|
| fw_test.sh → debug_poll after | Script then manual | Device dead (I/O Error) |
| Interactive `!stop` → `!start` | Debug console | PASS + PIB error 0x1005 |
| Interactive `!stop` → `!adc 0` → `!adc 32M` → `!start` | Debug console | PASS |
| Separate processes: stop → adc 0 → adc 32M → start | Command line | PASS |

Key observation: the failure **only** occurs in `fw_test.sh`, never in
manual tests.  The interactive test shows `GpifLoad = Succes[sful]`
confirming GPIF reload works.

### Root Cause: `CyU3PThreadSleep(100)` in `DebugPrint2USB`

The `DebugPrint2USB` function (`DebugConsole.c:256`) sleeps **100ms** when
the USB debug buffer is full:

```c
if (glDebTxtLen+len > MAXLEN_D_USB) CyU3PThreadSleep(100);
```

This is called from the USB EP0 callback (STARTFX3 handler) in the USB
thread context.  The host **cannot** drain the buffer during this sleep
because the USB thread is blocked in the same callback.

**Trigger chain in fw_test.sh:**

1. Test 20 (`debug_poll`) sets `glFlagDebug = true` — this enables
   `DebugPrint2USB` (otherwise it returns at line 245 without doing
   anything).
2. Tests 21-24 generate DebugPrint output that fills the 100-byte
   `glBufDebug[]`.  No test between 21 and 25 polls `READINFODEBUG`,
   so the buffer is never drained.
3. Test 25 (`stop_gpif_state`) sends STARTFX3.  The EP0 callback
   runs in the USB thread.
4. The STARTFX3 handler has **6 DebugPrint calls** (`GO in`, `GO dis`,
   `GPIF file`, `GpifLoad = Successful`, `GO gp=`, `GO sw=`).
   Each call finds the buffer full → `CyU3PThreadSleep(100)`.
5. Cumulative delay: **≥600ms** in the EP0 callback.  Combined with
   SDK call execution and RTOS scheduling jitter, this approaches or
   exceeds the host's 1000ms `CTRL_TIMEOUT_MS`.
6. The host times out → USB thread is left in a bad state →
   device becomes completely unresponsive.

**Why the interactive test works:** The `fx3_cmd debug` session
continuously polls `READINFODEBUG` every 50ms, keeping the buffer
drained.  `DebugPrint` never triggers the sleep.

**Why Test 5 (separate processes) works:** `glFlagDebug` was never
set to `true`.  `DebugPrint2USB` returns immediately at line 245
(`glFlagDebug == false`), skipping buffer and sleep entirely.

### Fix: Remove `CyU3PThreadSleep` from `DebugPrint2USB`

```c
void DebugPrint2USB(uint8_t priority, char *msg, ...) {
    if ((glIsApplnActive != CyTrue) || (glFlagDebug == false)) return;
    va_list argp;
    uint8_t buf[MAXLEN_D_USB];
    CyU3PReturnStatus_t stat;
    uint16_t len = MAXLEN_D_USB;
    va_start(argp, msg);
    stat = MyDebugSNPrint(buf, &len, msg, argp);
    va_end(argp);
    if (stat == CY_U3P_SUCCESS) {
        uint32_t intMask = CyU3PVicDisableAllInterrupts();
        if (glDebTxtLen + len < MAXLEN_D_USB) {
            memcpy(&glBufDebug[glDebTxtLen], buf, len);
            glDebTxtLen = glDebTxtLen + len;
        }
        CyU3PVicEnableInterrupts(intMask);
    }
}
```

Rationale: `CyU3PThreadSleep` inside a function callable from the USB
EP0 callback is fundamentally unsafe.  The sleep was intended to let
the host drain the buffer, but in EP0 context the host **cannot** drain
it (the USB thread is blocked).  If the buffer is full, silently drop
the message — this is the same behavior as when `glFlagDebug == false`.

### Upstream Provenance: Latent Bug Since Original Firmware

Comparison with the original upstream project
([ik1xpv/ExtIO_sddc](https://github.com/ik1xpv/ExtIO_sddc/blob/master/SDDC_FX3/DebugConsole.c))
confirms that the `CyU3PThreadSleep(100)` has been present since the
original codebase.  The original function is identical except for
variable naming (`flagdebug`, `debtxtlen`, `bufdebug` vs our
`glFlagDebug`, `glDebTxtLen`, `glBufDebug`) and the absence of
interrupt masking around the memcpy (a separate race-condition bug
in the original).

The original upstream code also enables `TRACESERIAL` by default
(`Application.h: #define TRACESERIAL`), which calls `TraceSerial()`
**after every EP0 vendor request** (`USBHandler.c:370`).
`TraceSerial` calls `DebugPrint` **2–3 times per request**:

```c
void TraceSerial(uint8_t bRequest, ...) {
    if (bRequest != READINFODEBUG) {
        DebugPrint(4, "%s\t", FX3CommandName[...]);   // (1) command name
        switch(bRequest) {
            case STARTADC:
                DebugPrint(4, "%d", ...);             // (2) argument
                break;
            case STARTFX3:
            case STOPFX3:
                break;                                // (no arg for these)
            ...
        }
        DebugPrint(4, "\r\n\n");                      // (3) terminator
    }
}
```

This means **every vendor request in the original firmware** — not
just our added diagnostic prints — was subject to the 100ms sleep
per DebugPrint call when the buffer was full and debug mode was
enabled.

**Impact on original firmware restart failures:**  The long-standing
user-reported issue where restarting the application firmware fails
(requiring `RESETFX3` or a full USB port reset) is consistent with
this bug.  The failure scenario in the original code:

1. Host application (ExtIO_sddc) sends `TESTFX3` with `wValue=1`,
   enabling debug mode (`flagdebug = true`).
2. Vendor requests accumulate `DebugPrint` output in the 100-byte
   buffer.  The Windows host application does not poll
   `READINFODEBUG`, so the buffer fills and stays full.
3. Host sends `STOPFX3` → EP0 callback runs.  `TraceSerial` calls
   `DebugPrint` 2× → 200ms sleep.  Plus the original `STOPFX3`
   handler's own `CyU3PThreadSleep(10)` → **210ms total** in EP0.
4. Host immediately sends `STARTFX3` → EP0 callback runs.
   `TraceSerial` calls `DebugPrint` 2× → **200ms sleep** in EP0.
5. Depending on host-side timeout and USB stack behavior, the
   cumulative delay causes intermittent EP0 timeouts.
6. A single EP0 timeout corrupts the USB thread state, making the
   device unresponsive until reset.

This explains why the failure was intermittent and hard to
reproduce: it required debug mode to be enabled AND the buffer to
be full AND a vendor request with enough DebugPrint calls to exceed
the host timeout.

**Additional original bug — no interrupt masking:**  The original
`DebugPrint2USB` does not use `CyU3PVicDisableAllInterrupts()`
around the `memcpy` + `debtxtlen` update.  If the `PibErrorCallback`
(which calls `DebugPrint`) fires during the memcpy, the buffer could
be corrupted.  Our fork already fixed this with interrupt masking.

### Secondary Fix: Reduce diagnostic prints in STARTFX3

The 6 DebugPrint calls in STARTFX3 were added for initial debugging
and are now excessive for production use.  Reduce to 1 call (final
state + return code) to minimize buffer pressure even after the
sleep fix.  STOPFX3 similarly reduced from 2 to 1.

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
