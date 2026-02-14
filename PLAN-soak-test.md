# Plan: FX3 Soak Test — Multi-Hour Wedge Exerciser

## Goal

Add a `soak` subcommand to `fx3_cmd` that randomly cycles through every
known way to wedge or stress the FX3, runs a health check after each
scenario, and continues for a user-specified duration (default: 1 hour).
The test should be deterministic given a seed, so failures are
reproducible.

## Existing infrastructure to build on

- `fx3_cmd.c` (1,669 lines): 23 test functions, USB helpers,
  `bulk_read_some()`, GETSTATS parser, debug console I/O, local command
  dispatch table
- `fw_test.sh` (603 lines): TAP runner for single-pass suite
- `dispatch_local_cmd()`: already dispatches by name — soak can reuse
  existing `do_test_*()` functions directly

## Architecture

```
fx3_cmd soak [hours] [seed]
         │
         ▼
┌─────────────────────────────────┐
│ soak_main()                     │
│  - parse duration & seed        │
│  - SIGINT handler for clean     │
│    shutdown with final report   │
│  - initial health check         │
│  - loop until duration expires: │
│    1. pick scenario (weighted)  │
│    2. run scenario function     │
│    3. run health check          │
│    4. update & print stats      │
│  - final report                 │
└─────────────────────────────────┘
```

## Scenarios

### Existing tests to wrap (no new test logic needed)

These already exist as `do_test_*()` functions.  The soak loop just
calls them repeatedly with health checks between calls.

| # | Scenario | Existing function | Weight | Notes |
|---|----------|-------------------|--------|-------|
| 1 | Stop/start cycling | `do_test_stop_start_cycle` | 20 | Core wedge-detection test; 5 cycles per call |
| 2 | DMA backpressure wedge+recovery | `do_test_wedge_recovery` | 15 | Most dangerous wedge; 2s stall each call |
| 3 | PIB overflow storm | `do_test_pib_overflow` | 5 | Interrupt saturation; slow (needs 300ms drain) |
| 4 | Debug buffer race | `do_test_debug_race` | 10 | 50 rapid interleaved cycles per call |
| 5 | Console buffer overrun | `do_test_console_fill` | 5 | Quick — ~100ms per call |
| 6 | EP0 overflow (wLength > 64) | `do_ep0_overflow` | 5 | Quick |
| 7 | OOB bRequest | `do_test_oob_brequest` | 5 | Quick |
| 8 | OOB SETARGFX3 wIndex | `do_test_oob_setarg` | 5 | Quick |
| 9 | PLL preflight (start without clock) | `do_test_pll_preflight` | 10 | Restores clock at end |

### New scenarios to add

| # | Scenario | Description | Est. lines |
|---|----------|-------------|------------|
| 10 | **Clock-pull mid-stream** | START streaming → STARTADC(0) to kill clock while GPIF is running → STOP → verify recovery via STOP+START+bulk read | ~45 |
| 11 | **Rapid frequency hopping** | STARTADC with 5 different frequencies (16/32/48/64/128 MHz) in quick succession, each followed by a brief START+read+STOP cycle | ~45 |
| 12 | **EP0 stall recovery** | Send OOB vendor request (gets STALL), immediately do TESTFX3 to verify EP0 still works | ~20 |
| 13 | **Back-to-back STOP** | Send STOPFX3 twice without intervening START; verify device handles redundant stop | ~20 |
| 14 | **Back-to-back START** | STARTFX3 twice without intervening STOP; verify second START doesn't crash (may STALL — that's fine) | ~25 |
| 15 | **I2C under streaming load** | START streaming → read Si5351 status via I2C (I2CRFX3) while data is flowing → STOP; verify both I2C and streaming are healthy | ~35 |
| 16 | **Sustained streaming** | START streaming at 64 MHz, read EP1 continuously for 30-60s, verify data count matches expected throughput (±50%) → STOP | ~45 |

## Health check (between every scenario)

A `soak_health_check()` function (~40 lines) that runs after each
scenario and flags cumulative degradation:

1. **TESTFX3** — device responds, hwconfig is still 0x04
2. **GETSTATS** — read all counters; compare to previous snapshot
   - GPIF state should be 0, 1, or 255 (not stuck in a read state)
   - Stack watermark (via debug "stack" command) still > 25% — run this
     every 10th scenario (it's slow: ~3s per call)
3. **EP0 round-trip** — implied by TESTFX3 success

The health check PASS/FAIL feeds back into the soak statistics.

## Soak statistics and reporting

### Per-scenario counters (struct)
```c
struct soak_scenario {
    const char *name;
    int (*func)(libusb_device_handle *);
    int weight;
    int runs;
    int pass;
    int fail;
};
```

### Status line (every 10 scenarios)
```
[00:12:34] cycle=247 pass=245 fail=2 | last=wedge_recovery(PASS) | dma=382716 pib=4 i2c=12 underrun=0
```

### Final report (on SIGINT or duration expiry)
```
=== SOAK TEST SUMMARY ===
Duration: 01:00:00  Seed: 12345  Cycles: 1847

Scenario              Runs  Pass  Fail
stop_start_cycle       370   370     0
wedge_recovery         277   275     2
freq_hopping           185   185     0
debug_race             185   185     0
...
TOTAL                 1847  1843     4

GETSTATS cumulative:
  dma_completions: 2,847,162
  pib_errors:      42
  i2c_failures:    92  (expected: all from stats_i2c NACK triggers)
  ep_underruns:    0
  health_checks:   1847/1847 passed

Result: 4 FAILURES in 1847 cycles (0.22% failure rate)
```

## Implementation plan (ordered steps)

### Step 1: New scenario functions (~235 lines)

Add 7 new `do_test_*()` functions to `fx3_cmd.c`:

- `do_test_clock_pull()` — clock-pull mid-stream
- `do_test_freq_hop()` — rapid frequency hopping
- `do_test_ep0_stall_recovery()` — EP0 stall then immediate use
- `do_test_double_stop()` — back-to-back STOP
- `do_test_double_start()` — back-to-back START
- `do_test_i2c_under_load()` — I2C read while streaming
- `do_test_sustained_stream()` — 30s continuous streaming

Each follows the existing pattern: setup → action → verify → cleanup →
return 0 (PASS) or 1 (FAIL).

Add to `local_cmds_noarg[]` table, `usage()`, and `main()` dispatch so
each is also callable standalone.

### Step 2: Soak harness (~170 lines)

Add to `fx3_cmd.c`:

- `soak_scenario[]` table with weights
- `soak_health_check()` function
- `soak_main()` — the outer loop:
  - Parse hours + seed from argv
  - Install SIGINT handler (set flag, don't exit)
  - Initial health check
  - Loop: weighted random pick → run → health check → stats update
  - Print status line every 10 cycles
  - Final summary report
- SIGINT handler: set `volatile sig_atomic_t soak_stop = 1`

### Step 3: CLI integration (~20 lines)

- Add `soak` to `usage()` help text
- Add `soak` case to `main()` dispatch (parse optional hours/seed args)

### Step 4: Shell wrapper `tests/soak_test.sh` (~60 lines)

- Firmware upload via rx888_stream (same as fw_test.sh)
- Device probe sanity check
- Invoke `fx3_cmd soak <hours> [seed]`
- Capture output
- Exit code from fx3_cmd (0 = all pass, 1 = any fail)

### Step 5: Build integration (~5 lines)

- Verify `tests/Makefile` already builds fx3_cmd (it does)
- No new dependencies — soak uses the same libusb-1.0

## Line count estimate

| Component | New lines |
|-----------|-----------|
| 7 new scenario functions | ~235 |
| Soak harness (loop, stats, health check, SIGINT) | ~170 |
| CLI integration (usage, main dispatch) | ~20 |
| Forward declarations, table entries | ~25 |
| `soak_test.sh` wrapper | ~60 |
| **Total** | **~510** |

This is a ~30% increase over the current 1,669 lines in fx3_cmd.c.

## What this does NOT cover

- Thermal monitoring (no hardware sensor accessible via USB)
- Power-cycle recovery (requires external relay/USB hub control)
- UART debug path (requires serial connection, not USB-testable)
- Signal integrity / ADC aliasing (requires external signal generator)
- USB re-enumeration stress (would need to close/reopen device handle;
  possible but adds significant complexity for marginal value)

## Testing the soak test itself

Before a long run, verify with a short sanity check:
```bash
# Run for 1 minute with a fixed seed
./fx3_cmd soak 0.016 42
```
This should complete ~50-100 cycles (each scenario takes 0.5-3s) and
exercise the full harness including the summary report.

## Ordering of implementation

All changes are in two files (`tests/fx3_cmd.c` and
`tests/soak_test.sh`).  Steps 1-3 can be done as a single commit
to fx3_cmd.c, Step 4 as a second commit adding the wrapper script.
