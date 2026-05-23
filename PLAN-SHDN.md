# PLAN — ADC low-power standby via SHDN when not streaming (issue #131)

## Goal

The RX888mk2 ADC has a shutdown control line (SHDN, FX3 GPIO 28,
active-high). Asserting it puts the ADC into a low-power standby state.
Today the firmware leaves the ADC powered whenever the device is enumerated,
even when no streaming session is active, drawing ≥330 mA and adding heat for
no benefit. This change drives SHDN from the streaming state so the ADC is
only powered while a stream is running.

## Behavior

| Event | SHDN action | Rationale |
|-------|-------------|-----------|
| Boot / GPIO init | assert standby (SHDN high) | No stream active yet |
| `STARTFX3` (0xAA) | wake (SHDN low) + short settle before GPIF runs | Stream starting |
| `STOPFX3` (0xAB) | assert standby (SHDN high) after GPIF/DMA stop | Stream stopped |

Manual `GPIOFX3` (0xAD) control of the SHDWN bit is unchanged — host can
still override. `STARTADC` (clock) and the watchdog recovery path are left
untouched: the ADC is only parked in standby when *not* streaming, and
recovery only runs mid-stream when the ADC is already awake.

## Changes

1. **`SDDC_FX3/radio/radio.h`** — declare
   `void rx888r2_AdcStandby(CyBool_t standby);`

2. **`SDDC_FX3/radio/rx888r2.c`**
   - Implement `rx888r2_AdcStandby()` as a thin wrapper over
     `CyU3PGpioSetValue(GPIO_SHDWN, standby)`.
   - In `rx888r2_GpioInitialize()`, assert standby after the existing pin
     setup (device is not streaming at boot).

3. **`SDDC_FX3/USBHandler.c`**
   - `STARTFX3`: after `GpifPreflightCheck()` passes, wake the ADC and sleep
     a short settle interval (`ADC_WAKEUP_SETTLE_MS`) before the GPIF state
     machine begins clocking data.
   - `STOPFX3`: after the GPIF/DMA stop+flush sequence, assert standby.

4. **`CHANGELOG.md`** — add an `[Unreleased]` entry.

5. **Docs** — note in `docs/api.md` / `docs/architecture.md` that firmware
   now drives SHDN from streaming state.

## Validation

- **Build**: standard `cd SDDC_FX3 && make`.
- **Functional**: `tests/fw_test.sh` — `hw_smoke`, `stop_gpif_state`, and
  `stop_start_cycle` exercise stop→start with data-flow checks; STARTFX3's
  wake makes these more robust. Optional: measure board current with a
  stream stopped (expect ~330 mA drop) vs. running.
- **Regression**: existing `device_quiesce` already clears SHDWN before
  streaming tests; STARTFX3 waking the ADC is additive and cannot leave it
  asleep during a stream. Watchdog recovery untouched.

## Risk / settle time

The settle delay before GPIF start guards against the ADC not having woken
when the first samples are clocked. A few ms on a start command is well
within the host's existing tolerance (STARTADC already polls PLL lock up to
100 ms).

## SHDN verification test (hardware harness)

A direct functional test that the SHDN line actually gates the ADC, run on
the hardware harness (`tests/fw_test.sh` + `rx888_stream`) — not GitHub CI,
which has no device attached.

Procedure:

1. `STARTADC` then `STARTFX3` (ADC awake, clock running). Take a `GETSTATS`
   snapshot; confirm CLK0 enabled + PLL locked, and record `dma_buf_count`.
2. **Leave the Si5351 clock running** and assert SHDN mid-stream via
   `GPIOFX3` with the SHDWN bit set. (The firmware only drives SHDN at
   start/stop, so a mid-stream manual assert sticks; STARTFX3 already woke
   the ADC, so this is the only way to reach "streaming with the ADC shut
   down.") Holding the clock up isolates the effect to SHDN rather than
   clock loss.
3. Take another `GETSTATS` snapshot after ~300 ms and inspect:
   - **Primary signal — DMA stall:** `dma_buf_count` stops advancing and the
     GPIF SM parks in a read/WAIT state. This is the expected result if the
     GPIF is externally clocked by the ADC sample clock: with no ADC clock
     edges the state machine cannot advance, while the FX3 CPU/RTOS stay
     alive on the internal clock. If observed, a frozen `dma_buf_count` with
     the clock confirmed up is a clean, automatable "ADC not clocking out"
     detector.
   - **Fallback signal — flat data:** if `dma_buf_count` keeps climbing,
     capture a buffer via `rx888_stream` and check that sample variance
     collapses toward zero (constant/DC) versus the live-noise baseline.
4. Clear SHDWN via `GPIOFX3`; confirm `dma_buf_count` resumes (and/or noise
   returns), proving the effect was reversible and tied to SHDN.

Note: a stopped clock and a shut-down ADC both produce flat/stalled data —
keeping the Si5351 verified-running throughout (step 2, via `GETSTATS`) is
what makes the result specific to SHDN. The absolute power claim (>=330 mA)
still requires a bench ammeter and is left as a manual bench check.

## Follow-up: documentation correctness issue

File a separate issue: `docs/wedge_detection.md` and `docs/architecture.md`
describe the GPIF as internally clocked (~100 MHz `CY_U3P_SYS_CLK/2`),
producing a "firehose of identical garbage samples" on clock loss. This is
contradicted by the firmware's own streaming watchdog
(`RunApplication.c`), which recovers by detecting that `glDMACount` *stops
advancing* — a detector that only works if clock loss stalls the DMA, not
if it keeps the buffers filling. The SHDN test above will settle the
clocking model empirically; the docs should be corrected to match.
