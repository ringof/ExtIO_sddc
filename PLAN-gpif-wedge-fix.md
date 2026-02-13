# Plan: Fix GPIF Wedge on STOPFX3 / STARTFX3

## Root Cause

STOPFX3 doesn't actually stop the GPIF state machine -- it only deasserts SW
input. If the SM is in a BUSY/WAIT state (blocked on DMA), it never checks SW
input and stays stuck. STARTFX3 then reasserts SW input into an already-wedged
SM. Compare with StopApplication() (USB disconnect), which correctly calls
`CyU3PGpifDisable(CyTrue)`.

## Approach: Incremental Phases

Each phase is one logical change + its tests. Build firmware after each firmware
change. Flash and run the test gate on hardware before moving to the next phase.
If a phase fails on hardware, we debug that phase in isolation before proceeding.

---

## Phase 0: Baseline Tests (test tooling only, no firmware changes)

**Goal:** Write the test subcommands in `fx3_cmd` and the `fw_test.sh` entries
that exercise stop/start behavior. Run them against the **current** firmware to
confirm they detect the broken behavior (expected: failures/wedge).

### 0a. Add `fx3_cmd` subcommand: `stop_gpif_state`

Sends STOPFX3, then reads GETSTATS and checks the GPIF SM state byte.

- **Current firmware expected result:** GPIF state is NOT 0 or 1 (RESET/IDLE) --
  the SM is still running or stuck, proving STOP didn't actually stop it.
- **After Phase 1 expected result:** GPIF state is 0 (RESET), proving
  GpifDisable worked.

### 0b. Add `fx3_cmd` subcommand: `stop_start_cycle`

Sequences: STARTADC(32 MHz) -> STARTFX3 -> brief stream read -> STOPFX3 ->
STARTFX3 -> brief stream read -> STOPFX3. Reads a few KB from the bulk
endpoint after each START to verify data is flowing. Repeats N times
(default 5).

- **Current firmware expected result:** Fails on the 2nd or 3rd cycle --
  bulk read times out because GPIF is wedged.
- **After Phase 2 expected result:** All N cycles complete.

### 0c. Add `fx3_cmd` subcommand: `pll_preflight`

Turns off ADC clock (STARTADC freq=0), then sends STARTFX3, then checks
whether the device accepted or rejected it.

- **Current firmware expected result:** STARTFX3 succeeds (no pre-flight
  check). This is the broken behavior.
- **After Phase 3 expected result:** STARTFX3 fails (STALL or isHandled=false).

### 0d. Add `fx3_cmd` subcommand: `wedge_recovery`

Starts streaming at 64 MHz, does NOT read EP1 (deliberately causes backpressure
and DMA stall), waits 2 seconds, then sends STOPFX3 + STARTFX3 and tries to
read data. Checks GETSTATS for recovery counter.

- **Current firmware expected result:** Device is wedged after the stall.
  STOPFX3+STARTFX3 does not recover. Bulk read times out.
- **After Phase 2 expected result:** Stop/start recovers the stream.
- **After Phase 4 expected result:** Watchdog may auto-recover before the
  host even sends STOP (check glCounter[1] in GETSTATS).

### 0e. Add `fw_test.sh` entries

Add test entries that call the new subcommands. Place them in the test
sequence **before** `pib_overflow` (which is known to wedge the device).

### Test gate 0

Build `fx3_cmd`. Run the new tests against the **current** firmware:
- `stop_gpif_state`: expect FAIL (SM not in IDLE)
- `stop_start_cycle`: expect FAIL (wedges mid-cycle)
- `pll_preflight`: expect FAIL (START succeeds without clock)
- `wedge_recovery`: expect FAIL (no recovery)

These failures confirm the tests are detecting the real problems.

---

## Phase 1: Fix STOPFX3

**Goal:** Make STOPFX3 actually stop the GPIF state machine.

**File:** `SDDC_FX3/USBHandler.c`, STOPFX3 case (~line 283)

**Change:**
```c
case STOPFX3:
    CyU3PUsbLPMEnable();
    CyU3PUsbGetEP0Data(wLength, glEp0Buffer, NULL);
    CyU3PGpifControlSWInput(CyFalse);
    CyU3PGpifDisable(CyFalse);          // NEW: force-stop SM, keep waveform config
    CyU3PDmaMultiChannelReset(&glMultiChHandleSlFifoPtoU);
    CyU3PUsbFlushEp(CY_FX_EP_CONSUMER);
    isHandled = CyTrue;
    break;
```

- Add `CyU3PGpifDisable(CyFalse)` after SW input deassert. This force-stops
  the SM even if it's stuck in BUSY/WAIT. `CyFalse` keeps waveform memory.
- Remove the 10ms sleep -- `GpifDisable` is synchronous.

**What we are NOT changing yet:** STARTFX3 is unchanged. This means a
STOP+START cycle will still fail (the SM is now properly stopped but STARTFX3
doesn't restart it). That's expected and tests will confirm it.

### Test gate 1

Flash the Phase 1 firmware. Run:

- `stop_gpif_state`: **expect PASS** -- SM state should be 0 (RESET) after STOP.
- `stop_start_cycle`: expect FAIL -- START doesn't restart the SM yet.
- All pre-existing `fw_test.sh` tests: expect PASS (STOP-only change shouldn't
  break enumeration, GPIO, ADC, attenuator, etc.)

**What this proves:** GpifDisable actually stops the SM. We can observe the
state via GETSTATS. If this fails, we debug the STOP path in isolation before
touching START.

---

## Phase 2: Fix STARTFX3

**Goal:** Make STARTFX3 restart the GPIF SM from a clean state.

**File:** `SDDC_FX3/USBHandler.c`, STARTFX3 case (~line 270)

**Change:**
```c
case STARTFX3:
    CyU3PUsbLPMDisable();
    CyU3PUsbGetEP0Data(wLength, glEp0Buffer, NULL);
    CyU3PDmaMultiChannelReset(&glMultiChHandleSlFifoPtoU);
    apiRetStatus = CyU3PDmaMultiChannelSetXfer(
        &glMultiChHandleSlFifoPtoU, FIFO_DMA_RX_SIZE, 0);
    if (apiRetStatus == CY_U3P_SUCCESS)
    {
        apiRetStatus = CyU3PGpifSMStart(0, 0);   // NEW: restart SM
        if (apiRetStatus == CY_U3P_SUCCESS)
        {
            CyU3PGpifControlSWInput(CyTrue);
            isHandled = CyTrue;
        }
    }
    break;
```

- Add `CyU3PGpifSMStart(0, 0)` to restart the SM from state 0. Since
  STOPFX3 used `GpifDisable(CyFalse)`, waveform data is still loaded --
  only SMStart is needed, not GpifLoad.
- Sequence: DMA reset -> DMA xfer setup -> SM start -> SW input assert.
  DMA must be ready before the SM starts requesting buffers.
- Remove the leading `CyU3PGpifControlSWInput(CyFalse)` -- the SM is
  already disabled after STOPFX3.

### Test gate 2

Flash the Phase 2 firmware. Run:

- `stop_gpif_state`: **expect PASS** (unchanged from Phase 1)
- `stop_start_cycle`: **expect PASS** -- this is the key test. N cycles of
  STOP+START should all produce streaming data.
- `wedge_recovery`: **expect PASS** -- after deliberate DMA backpressure,
  STOP+START should recover the stream.
- All pre-existing `fw_test.sh` tests: expect PASS.
- **Streaming tests**: run `fw_test.sh --firmware ... ` with streaming enabled.
  Verify the streaming tests still pass (they do a single upload+stream cycle).

**What this proves:** The core STOP/START cycle is fixed. The device can be
stopped and restarted repeatedly without wedging, even after DMA backpressure.

---

## Phase 3: PLL Lock Pre-Flight Check in STARTFX3

**Goal:** Refuse to start streaming if the ADC clock isn't running.

**File:** `SDDC_FX3/USBHandler.c`, STARTFX3 case (add before DMA reset)

**Change:** Add at the top of the STARTFX3 handler, after GetEP0Data:
```c
    {
        uint8_t si_status = 0;
        I2cTransfer(0x00, 0xC0, 1, &si_status, CyTrue);
        if (si_status & 0x20) {
            DebugPrint(4, "STARTFX3: PLL_A unlocked, aborting");
            isHandled = CyFalse;
            break;
        }
    }
```

This reads Si5351 register 0 (device status) and checks bit 5 (LOL_A = PLL A
loss-of-lock). If the PLL is unlocked, STARTFX3 is rejected.

### Test gate 3

Flash the Phase 3 firmware. Run:

- `pll_preflight`: **expect PASS** -- STARTFX3 with clock off is rejected.
- `stop_start_cycle`: **expect PASS** -- normal cycling still works (PLL is
  locked during these tests because STARTADC was called first).
- `wedge_recovery`: expect PASS.
- All pre-existing `fw_test.sh` tests: expect PASS.

**What this proves:** The pre-flight check correctly blocks START when the
clock is bad, without interfering with normal operation.

---

## Phase 4: GPIF Watchdog in Main Loop

**Goal:** Auto-detect and recover from a GPIF wedge without host intervention.

**File:** `SDDC_FX3/RunApplication.c`, ApplicationThread main loop (~line 202)

**Change:** Add after the event-processing block inside the `while(1)` loop:

```c
if (glIsApplnActive)
{
    static uint32_t prevDMACount = 0;
    static uint8_t  stallCount = 0;

    uint32_t curDMA = glDMACount;
    if (curDMA == prevDMACount && curDMA > 0)
    {
        uint8_t gpifState = 0xFF;
        CyU3PGpifGetSMState(&gpifState);

        if (gpifState == 5 || gpifState == 7 ||
            gpifState == 8 || gpifState == 9)
        {
            stallCount++;
            if (stallCount >= 3)  // 300ms in BUSY/WAIT
            {
                DebugPrint(4, "\r\nGPIF WEDGE (state=%d), recovering", gpifState);
                CyU3PGpifControlSWInput(CyFalse);
                CyU3PGpifDisable(CyFalse);
                CyU3PDmaMultiChannelReset(&glMultiChHandleSlFifoPtoU);
                CyU3PUsbFlushEp(CY_FX_EP_CONSUMER);

                uint8_t si_status = 0;
                I2cTransfer(0x00, 0xC0, 1, &si_status, CyTrue);
                if (si_status & 0x20)
                {
                    DebugPrint(4, " PLL_A unlocked, waiting for host");
                    glCounter[1]++;
                }
                else
                {
                    DebugPrint(4, " PLL OK, auto-restart");
                    CyU3PDmaMultiChannelSetXfer(
                        &glMultiChHandleSlFifoPtoU, FIFO_DMA_RX_SIZE, 0);
                    CyU3PGpifSMStart(0, 0);
                    CyU3PGpifControlSWInput(CyTrue);
                }
                stallCount = 0;
                prevDMACount = 0;
                glDMACount = 0;
            }
        }
        else
            stallCount = 0;
    }
    else
    {
        stallCount = 0;
        prevDMACount = curDMA;
    }
}
```

Logic: every 100ms, compare glDMACount to previous value. If it hasn't
advanced and the GPIF SM is in a BUSY/WAIT state, increment a stall counter.
After 3 consecutive hits (300ms), tear down and rebuild the streaming
pipeline. Check PLL lock to decide whether to auto-restart or wait for
the host.

This requires `glMultiChHandleSlFifoPtoU` to be accessible from
RunApplication.c (it's defined in StartStopApplication.c). Add an `extern`
declaration.

### Test gate 4

Flash the Phase 4 firmware. Run:

- `wedge_recovery`: **expect PASS** with a twist -- check GETSTATS for
  `glCounter[1] > 0`, confirming the watchdog fired and auto-recovered.
  Also check debug output for "GPIF WEDGE" string.
- `stop_start_cycle`: expect PASS.
- `pll_preflight`: expect PASS.
- `stop_gpif_state`: expect PASS.
- All pre-existing `fw_test.sh` tests: expect PASS.
- **pib_overflow re-test**: This test deliberately wedges the GPIF by not
  reading EP1. With the watchdog, the firmware should auto-recover after
  ~300ms. The test should still detect PIB errors in debug output, and
  the device should be responsive afterward (it may already be today, but
  now it's by design rather than luck).

**What this proves:** The firmware can self-heal from a GPIF stall without
any host action.

---

## Summary: What We Build and When

| Phase | Firmware change | Test tooling | Key test gate |
|-------|----------------|--------------|---------------|
| 0 | None | 4 new fx3_cmd subcommands + fw_test.sh entries | All 4 tests FAIL (baseline) |
| 1 | STOPFX3: add GpifDisable | None | `stop_gpif_state` PASS |
| 2 | STARTFX3: add GpifSMStart | None | `stop_start_cycle` + `wedge_recovery` PASS |
| 3 | STARTFX3: PLL pre-flight | None | `pll_preflight` PASS |
| 4 | Main loop: watchdog | None | `wedge_recovery` with glCounter[1] > 0 |

## Files Modified

| File | Phase | Change |
|------|-------|--------|
| `tests/fx3_cmd.c` | 0 | 4 new test subcommands |
| `tests/fw_test.sh` | 0 | 4 new test entries |
| `SDDC_FX3/USBHandler.c` | 1,2,3 | Fix STOPFX3, STARTFX3, PLL pre-flight |
| `SDDC_FX3/RunApplication.c` | 4 | GPIF watchdog in main loop |

## What Is NOT Changed

- **GPIF state machine (SDDC_GPIF.h)** -- no SM redesign needed.
- **StopApplication / StartApplication** -- the USB connect/disconnect path
  is already correct.
- **DMA channel configuration** -- we reset and re-arm, not destroy/recreate.

## Risk Assessment

- Each phase is a single logical change with a clear pass/fail test.
- If any phase fails on hardware, we stop and debug that phase in isolation.
- The worst case for any phase is that it doesn't fix the wedge, which
  leaves us no worse than today -- the device is already broken for stop/start.
- `CyU3PGpifDisable(CyFalse)` + `CyU3PGpifSMStart()` is the SDK-recommended
  pattern for stopping and restarting the GPIF without tearing down PIB.
