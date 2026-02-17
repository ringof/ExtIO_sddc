# Plan: Watchdog Recovery Cap

## Problem

When a host application crashes or is killed without sending STOPFX3, the
device is left streaming with no consumer.  The watchdog detects the stall
and recovers, but since no consumer will ever appear, it loops forever —
tearing down and rebuilding GPIF/DMA every ~1 second indefinitely.  This
wastes FX3 CPU time, hammers I2C, and can cause transient EP0 timeouts
under sustained churn.

## Design

Add a configurable maximum consecutive-recovery count to the watchdog.
After that many recoveries with no DMA progress between them, the watchdog
stops attempting recovery and leaves GPIF/DMA torn down.  The device
remains EP0-responsive; a new STARTFX3 from the host resets the counter
and resumes normal operation.

### Value semantics

| Value | Meaning |
|-------|---------|
| 0     | Unlimited recovery (original behaviour, no cap) |
| 1–255 | Max consecutive recoveries before giving up |
| Default | **5** (~1.5 s of recovery — transient stalls clear in 1–2) |

### Reset conditions

The consecutive-recovery counter resets to 0 when:

- **DMA resumes** — `curDMA != prevDMACount` (host is consuming)
- **STARTFX3 received** — host is starting a new streaming session
- **STOPFX3 received** — explicit stop from host

### State when max reached

- GPIF disabled, DMA reset (left from the last recovery teardown)
- Watchdog prints: `WDG: recovery limit (%d), waiting for STARTFX3`
- No further recovery attempts until a reset condition occurs
- Device stays EP0-responsive for configuration and restart

## File Changes

### 1. `SDDC_FX3/protocol.h`

- Add `WDG_MAX_RECOV = 14` to `enum ArgumentList`
- Bump `SETARGFX3_LIST_COUNT` from 14 → 15
- Add `#define WDG_MAX_RECOVERY_DEFAULT 5`

### 2. `SDDC_FX3/DebugConsole.c`

- Append `"WDG_MAX_RECOV"` to `SETARGFX3List[]`

### 3. `SDDC_FX3/RunApplication.c`

- Add two globals:
  - `uint8_t glWdgMaxRecovery = WDG_MAX_RECOVERY_DEFAULT;`
  - `uint8_t glWdgRecoveryCount = 0;`
- In watchdog recovery block:
  - Increment `glWdgRecoveryCount` on each recovery
  - Before attempting recovery: if `glWdgMaxRecovery > 0` and
    `glWdgRecoveryCount >= glWdgMaxRecovery`, skip recovery, print
    limit message, and set `stallCount = 0` to stop further polling
  - When DMA resumes (existing `curDMA != prevDMACount` branch):
    reset `glWdgRecoveryCount = 0`

### 4. `SDDC_FX3/USBHandler.c`

- `extern` the two new globals
- SETARGFX3 switch: add `case WDG_MAX_RECOV:` that writes `wValue`
  to `glWdgMaxRecovery` (clamp wValue to uint8_t range)
- STARTFX3 handler: reset `glWdgRecoveryCount = 0`
- STOPFX3 handler: reset `glWdgRecoveryCount = 0`

### 5. `tests/fx3_cmd.c`

- Add `#define WDG_MAX_RECOV 14`
- Add `do_wdg_max` subcommand: `set_arg(h, WDG_MAX_RECOV, val)`
- Add to help text and dispatch table

### 6. `tests/README.md`

- Document new `wdg_max` command under fx3_cmd basic commands

## Not Changed

- **GETSTATS layout**: stays at 20 bytes.  `glCounter[2]` (streaming
  fault counter) already reflects total recovery count.  The new
  consecutive counter is observable via debug console output.
- **fw_test.sh**: no new automated test for this feature (would require
  deliberate no-consumer streaming to validate the cap — same pattern
  as the manual debug-console sequence).
