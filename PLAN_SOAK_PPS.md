# Plan: Soak PPS — continuous 1 Hz marker injection during soak

## Goal

Add a `--pps` option to `fx3_cmd soak` that runs a background 1 Hz
GPIO18 (BIAS_VHF, bit 9) toggle for the duration of the soak.  This
simulates a real PPS timing signal arriving at the RX888 while the
device is subjected to random scenario stress.

## Mechanism

A `pthread` launched at soak start:
- Fires once per second (monotonic clock + `nanosleep`)
- Reads a shared `gpio_base` word, ORs in bit 9 (high), writes
  `GPIOFX3`; short dwell; clears bit 9 (low), writes `GPIOFX3`
- EP0 vendor commands are non-blocking on the device side; no
  locking needed for the USB transfer itself

## Issue 1: GPIO bit 9 reservation

`GPIOFX3` is a write-all register — any scenario that writes GPIO
clobbers all bits, including bit 9.  While `--pps` is active:

- **Bit 9 is reserved for the PPS thread.**  No scenario may set or
  clear it.
- All `GPIOFX3` writes from scenario code (and from the soak
  harness itself) must go through a helper that masks out bit 9
  before writing:
  ```c
  /* When soak_pps_active, bit 9 is owned by the PPS thread. */
  static void gpio_write(libusb_device_handle *h, uint32_t bits) {
      if (soak_pps_active)
          bits = (bits & ~0x200u) | (gpio_pps_bit & 0x200u);
      cmd_u32(h, GPIOFX3, bits);
  }
  ```
- Scenarios that intentionally test GPIO extremes (`gpio_extremes`)
  will have bit 9 masked — acceptable because the PPS reservation is
  an explicit operator choice via `--pps`.
- The shared `gpio_pps_bit` variable is written only by the PPS
  thread (0x200 during the high phase, 0x000 during the low phase).

### Where to intercept

The simplest insertion point: `cmd_u32()` is already the bottleneck
for all vendor-command writes.  Add a `GPIOFX3`-specific mask there
rather than auditing every call site:

```c
static int cmd_u32(libusb_device_handle *h, uint8_t cmd, uint32_t val) {
    if (cmd == GPIOFX3 && soak_pps_active)
        val = (val & ~0x200u) | (__atomic_load_n(&gpio_pps_bit, __ATOMIC_RELAXED) & 0x200u);
    // ... existing transfer code ...
}
```

This covers all existing and future callers with zero churn.

## Issue 2: Device handle staleness

`test_health_recovery` and `test_main_recovery` reboot the FX3 and
re-acquire the USB device handle.  The soak loop already propagates
the new handle via `h_inout`.  The PPS thread needs to follow:

- A shared `volatile libusb_device_handle *pps_handle` pointer,
  atomically updated by the soak loop after each scenario that may
  re-acquire (i.e. after every `scenarios[sel].func(h)` call, just
  set `pps_handle = h`).
- The PPS thread reads `pps_handle` on each tick.  If `NULL`, it
  skips the tick (device is mid-reset).
- Before a recovery scenario, the soak loop sets `pps_handle = NULL`
  to suppress PPS during the reset window.  After re-acquire, it
  sets `pps_handle = h`.

Simplified: just update `pps_handle = h` unconditionally after every
scenario returns (it's already in the soak loop).  Recovery scenarios
take ~15 s; the PPS thread will get one stale-handle error at most,
which `libusb_control_transfer` returns as `LIBUSB_ERROR_NO_DEVICE`.
The thread can silently swallow that and retry next tick.

## Implementation steps

1. **Add globals**: `soak_pps_active`, `gpio_pps_bit`, `pps_handle`
2. **Modify `cmd_u32()`**: mask bit 9 on `GPIOFX3` when PPS active
3. **Write `soak_pps_thread()`**: 1 Hz toggle loop with
   `clock_nanosleep`, reads `pps_handle`, swallows
   `LIBUSB_ERROR_NO_DEVICE`
4. **Parse `--pps` flag** in `soak_main()`
5. **Launch/join thread**: `pthread_create` before soak loop,
   `pthread_join` after (set `soak_stop` to signal exit)
6. **Update `pps_handle`** after each scenario call
7. **Update soak banner**: print "PPS: 1 Hz GPIO18" when active
8. **Update help text**

## Testing

```bash
# Basic: 5-minute soak with PPS
./tests/fx3_cmd soak 0.083 --pps

# Verify PPS fires — look for GETSTATS pps_count advancing
# (only meaningful with Config B firmware + loopback)

# Verify gpio_extremes doesn't clobber bit 9:
./tests/fx3_cmd soak 0.083 --pps -w gpio_extremes=50
```

## Out of scope

- Making `pps_inject` (the ramp test) PPS-aware — it has its own
  GPIO18 logic and runs independently.
- Verifying marker content in the data stream — that's Phase 4b/5.
