# PLAN: Fix test failures — revert STARTFX3 + fix gpio_extremes state leak

## Problem

6 of 38 tests fail: stop_start_cycle, wedge_recovery, pib_overflow, stream (x3).
Two independent root causes:

1. **gpio_extremes leaves ADC in shutdown** — test 28 writes GPIO pattern
   `0x0001FFFF` which sets SHDWN (bit 5), putting the LTC2208 ADC to sleep.
   No cleanup restores GPIO, so all later streaming tests get 0 bytes.

2. **STARTFX3 handler was modified in a 3-commit chain** (6b35bcc → da14a72 →
   e4dc1a2) that added `CyU3PUsbResetEp` and reordered SetXfer/StartGPIF.
   These changes were chasing a symptom that was actually caused by issue #1.
   Revert to the pre-6b35bcc state to restore known-working firmware behavior.

## Changes

### 1. [DONE] Revert STARTFX3 handler to pre-6b35bcc state (USBHandler.c)

Undid the 3-commit chain's changes to the STARTFX3 case:
- **Removed** `CyU3PUsbResetEp(CY_FX_EP_CONSUMER)` and its comment
- **Restored original order**: SetXfer → StartGPIF → FW_TRG
  (was StartGPIF → SetXfer → FW_TRG)

Verified: `git diff 6b35bcc~1 -- SDDC_FX3/USBHandler.c` shows zero diff.

Test-side changes from #78 chain (libusb_clear_halt in open_rx888, GETSTATS
diagnostics) are **kept** — they are host-side fixes independent of firmware.

### 2. [DONE] Restore GPIO in `do_test_gpio_extremes()` (fx3_cmd.c)

After the extreme-pattern loop, restore GPIO to known-good state:
```c
cmd_u32(h, GPIOFX3, 0x0800);  /* LED_BLUE — matches rx888r2_GpioInitialize() */
```

### 3. [DONE] Add GPIO restore to `device_quiesce()` in fw_test.sh

Safety net: restore GPIO to `LED_BLUE` (0x0800) during inter-test cleanup.
```bash
"$FX3_CMD" gpio 0x0800 >/dev/null 2>&1 || true
```

### 4. [DONE] Add GPIO restore to soak runner quiesce (fx3_cmd.c)

Same GPIO restore in the soak runner's inter-scenario cleanup.

### 5. [DONE] Add `hw_smoke` test (fw_test.sh + fx3_cmd.c)

Minimal test after GPIO-manipulating tests: set known-good GPIO, set ADC clock,
START, read data, STOP.  Directly verifies the RX888mk2 "still works" before
the complex streaming tests.  TAP plan bumped 35 → 36.

## Build/Validation

- Rebuild firmware: `cd SDDC_FX3 && make`
- Rebuild tests: `cd tests && make`
- Flash new firmware and run: `./fw_test.sh`
- All 38+ tests should pass
- The 6 previously-failing tests should now pass

## Regression Check

- stop_start_cycle: 5 STOP→START cycles with data each time
- wedge_recovery: DMA backpressure then recovery
- pib_overflow: PIB error detected at 64 MS/s
- stream: full streaming capture with non-zero data
