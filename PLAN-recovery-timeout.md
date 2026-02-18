# Plan: Watchdog Recovery Cap + STOPFX3 Hardening

Covers: [#73](https://github.com/ringof/ExtIO_sddc/issues/73),
[#77](https://github.com/ringof/ExtIO_sddc/issues/77)

## Problems

### #73 — Watchdog loops forever when host crashes without STOPFX3

When a host application crashes or is killed without sending STOPFX3, the
device is left streaming with no consumer.  The watchdog detects the stall
and recovers, but since no consumer will ever appear, it loops forever —
tearing down and rebuilding GPIF/DMA every ~1 second indefinitely.  This
wastes FX3 CPU time, hammers I2C, and can cause transient EP0 timeouts
under sustained churn.

### #77 — Rapid START/STOP cycling causes hard lockup

50× STARTFX3/STOPFX3 with ~1ms gaps and zero bulk reads causes a hard
device lockup on the next STARTADC.  Two root causes:

1. **`glDMACount` not reset in STOPFX3** — the watchdog sees the stale
   count from the previous session, false-fires recovery, and races with
   the EP0 callback setting up the next STARTFX3.
2. **No DMA settle time in STOPFX3** — `CyU3PDmaMultiChannelReset()`
   followed immediately by `SetXfer()` in the next STARTFX3 (~1ms later)
   can accumulate DMA controller state corruption over many iterations.

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
- STOPFX3 handler:
  - Reset `glWdgRecoveryCount = 0`
  - Add `glDMACount = 0` — prevents watchdog false-positive on stale
    count from previous session (#77 root cause 1)
  - Add `CyU3PThreadSleep(1)` after `CyU3PDmaMultiChannelReset()` —
    lets DMA controller quiesce before the next STARTFX3 re-arms it
    (#77 root cause 2)

### 5. `tests/fx3_cmd.c`

- Add `#define WDG_MAX_RECOV 14`
- Add `do_wdg_max` subcommand: `set_arg(h, WDG_MAX_RECOV, val)`
- Add to help text and dispatch table

### 6. `tests/README.md`

- Document new `wdg_max` command under fx3_cmd basic commands

## Validation

After applying these changes:

1. **Soak test** — `./fx3_cmd soak 1` should show zero `STARTFX3`
   failures from the `rapid_start_stop` scenario (previously ~3% fail).
2. **Abandoned-stream test** — `./fx3_cmd abandoned_stream` should see
   recovery count plateau at `WDG_MAX_RECOV` instead of climbing
   indefinitely.
3. **Normal streaming** — `./fx3_cmd sustained_stream` unaffected.

## Not Changed

- **GETSTATS layout**: stays at 20 bytes.  `glCounter[2]` (streaming
  fault counter) already reflects total recovery count.  The new
  consecutive counter is observable via debug console output.
- **fw_test.sh**: no new automated test for this feature (would require
  deliberate no-consumer streaming to validate the cap — same pattern
  as the manual debug-console sequence).
