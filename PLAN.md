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
