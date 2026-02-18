# Plan: `--reload-interval` for soak_test.sh

## Problem

The soak test uploads firmware **once** at startup, then runs all scenarios
against that single running instance.  This never tests whether a *freshly
loaded* firmware image handles stress correctly — only that an already-warm
instance survives.  A fresh load goes through the full `CyU3PUsbStart` /
`CyU3PGpifLoad` / DMA-channel-create path, which may behave differently from
a STOPFX3/STARTFX3 cycle on a device that's been running for hours.

## Design

### New argument

```
--reload-interval N    Every N scenarios, reset device to DFU, re-upload
                       firmware, and resume the soak.  Default: 0 (disabled).
```

### Why the shell wrapper owns this, not fx3_cmd

When `usbreset` fires, the FX3 drops its RAM-loaded firmware and
re-enumerates as PID 0x00F3 (bootloader).  `fx3_cmd`'s libusb handle
becomes invalid — it cannot survive a device reset.  The firmware upload
requires `rx888_stream`, which is an external binary.  Therefore the
reload cycle **must** live in `soak_test.sh`, and the soak run must be
split into chunks.

### New fx3_cmd soak option: `--max-scenarios`

`fx3_cmd soak` currently runs until its time budget expires or Ctrl-C.
To support chunked runs, add a third optional positional argument:

```
fx3_cmd soak [hours] [seed] [max_scenarios]
```

When `max_scenarios > 0`, the soak loop exits cleanly (with stats) after
completing that many scenarios, **even if time remains**.  Exit code is
still 0/1 based on pass/fail, same as today.

This keeps the C code simple — no IPC, no signal protocol, just "run N
then stop."

### Seed continuity

Each chunk uses a deterministic seed so the full sequence is reproducible:

```
chunk_seed = original_seed + chunk_number
```

The shell passes `$((SEED + chunk))` to each `fx3_cmd` invocation.
Not identical to a single unbroken run with the same seed (the PRNG
state resets each chunk), but reproducible given the same
`--seed` + `--reload-interval` combination.

### Stats aggregation

Each `fx3_cmd soak` chunk prints its own summary table.  The shell
wrapper:
1. Captures each chunk's exit code (pass/fail).
2. After all chunks, prints a one-line aggregate:
   `Reload summary: K reloads, M chunks passed, N total scenarios`.
3. Individual chunk summaries remain visible in scrollback.

No machine-readable output parsing needed — keep it simple.

## Changes

### 1. `fx3_cmd.c` — add `max_scenarios` to `soak_main`

**File:** `tests/fx3_cmd.c`, function `soak_main` (~line 2117)

- Parse optional third positional arg `max_scenarios` (default 0 = unlimited).
- In the `while (!soak_stop)` loop, add a check:
  ```c
  if (max_scenarios > 0 && total_cycles >= max_scenarios) break;
  ```
- Update the usage string to document the new argument.
- Print `max_scenarios` in the soak header if non-zero.

**Impact:** Minimal — one new variable, one new `break` condition.

### 2. `soak_test.sh` — add `--reload-interval` and chunk loop

**File:** `tests/soak_test.sh`

- New variable: `RELOAD_INTERVAL=0`
- New option in the `while/case` parser: `--reload-interval) RELOAD_INTERVAL="$2"; shift 2 ;;`
- Replace the single `fx3_cmd soak` call with a loop:

```bash
CHUNK=0
CHUNK_FAILS=0
OVERALL_SCENARIOS=0

while true; do
    # Calculate remaining time
    ELAPSED=$(( $(date +%s) - START_EPOCH ))
    REMAINING_SEC=$(( TOTAL_SEC - ELAPSED ))
    if (( REMAINING_SEC <= 0 )); then break; fi
    REMAINING_HOURS=$(awk "BEGIN{printf \"%.4f\", $REMAINING_SEC/3600}")

    CHUNK_SEED=$(( BASE_SEED + CHUNK ))

    # Build fx3_cmd args
    SOAK_ARGS="$REMAINING_HOURS $CHUNK_SEED"
    if (( RELOAD_INTERVAL > 0 )); then
        SOAK_ARGS="$SOAK_ARGS $RELOAD_INTERVAL"
    fi

    "$FX3_CMD" soak $SOAK_ARGS || CHUNK_FAILS=$((CHUNK_FAILS + 1))
    CHUNK=$((CHUNK + 1))

    # If reload disabled or time expired, we're done
    if (( RELOAD_INTERVAL == 0 )); then break; fi

    ELAPSED=$(( $(date +%s) - START_EPOCH ))
    if (( ELAPSED >= TOTAL_SEC )); then break; fi

    # ---- Reload cycle ----
    echo "# ---- Reload cycle $CHUNK: reset + firmware upload ----"
    usbreset "$APP_VID_PID" &>/dev/null || true
    sleep 2

    # Wait for bootloader PID (up to 5s)
    for i in 1 2 3 4 5; do
        lsusb -d "$BOOT_VID_PID" &>/dev/null && break
        sleep 1
    done

    if ! lsusb -d "$BOOT_VID_PID" &>/dev/null; then
        echo "# Error: device not in bootloader after reset — aborting"
        break
    fi

    # Re-upload firmware (same as startup path)
    timeout 15 "$RX888_STREAM" -f "$FIRMWARE" -s 32000000 \
        > /dev/null 2>/dev/null &
    UPLOAD_PID=$!
    sleep 4
    kill "$UPLOAD_PID" 2>/dev/null || true
    wait "$UPLOAD_PID" 2>/dev/null || true
    sleep 2

    if ! lsusb -d "$APP_VID_PID" &>/dev/null; then
        echo "# Error: device not at app PID after reload — aborting"
        break
    fi
    echo "# Reload $CHUNK complete — resuming soak"
done
```

- After the loop, print the aggregate summary and exit with
  appropriate code.
- Update the header comment and `--help` output.

### 3. `tests/README.md` — document the new option

Add `--reload-interval N` to the soak_test.sh options table with a note
that it's disabled by default and extends test time by ~15s per reload.

## What does NOT change

- `fw_test.sh` — unrelated, no changes.
- All existing soak scenarios — untouched.
- Default behavior (`--reload-interval` absent or 0) — identical to today.
- `fx3_cmd soak` with no third argument — identical to today.

## Estimated time cost per reload

| Step                    | Time    |
|-------------------------|---------|
| `usbreset`              | ~0.5s   |
| Wait for bootloader PID | 1-3s    |
| Firmware upload          | ~4s     |
| Post-upload settle       | ~2s     |
| **Total per reload**     | **~10s** |

With `--reload-interval 50` on a 1-hour soak (~300 scenarios), expect
~6 reloads adding ~60s total — negligible.

## Validation

1. `soak_test.sh --firmware ... --hours 0.05 --reload-interval 10` — verify
   it runs ~10 scenarios, reloads, runs ~10 more, and the device survives.
2. `soak_test.sh --firmware ... --hours 0.01` — verify default (no reload)
   behavior is unchanged.
3. `fx3_cmd soak 0.1 42 5` — verify it exits after exactly 5 scenarios.

## Regression

- `soak_test.sh --firmware ... --hours 0.05` with no `--reload-interval` —
  must behave identically to current code (single unbroken run).
- `fw_test.sh` — must still pass unmodified.
