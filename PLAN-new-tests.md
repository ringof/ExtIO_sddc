# Plan: New USB Command Tests

## Goal

Fill coverage gaps identified by audit of `tests/fx3_cmd.c` and `tests/fw_test.sh`.
All 11 vendor commands already have basic happy-path coverage. The gaps are in
boundary conditions, counter semantics, data integrity, and stress paths.

---

## New tests — fx3_cmd.c subcommands

Each test below becomes:
- A `do_test_<name>()` function in `fx3_cmd.c`
- An entry in `local_cmds_noarg[]` (lines 403-433)
- A forward declaration (lines 374-395)
- Where noted, also an entry in the soak `scenarios[]` table (lines 2913-2941)

---

### T1 — `vendor_rqt_wrap` (counter wraparound)

**What**: `glVendorRqtCnt` is `uint8_t`. Send 260 TESTFX3 commands in a loop,
parse byte 3 of each response. Assert the counter wraps from 255 to 0.

**Why**: Counter wrap has never been tested; a firmware change that widened the
counter or broke the increment path would go unnoticed.

**Soak**: No (deterministic, not useful to repeat randomly).

---

### T2 — `stale_vendor_codes` (dead-zone bRequest values)

**What**: Send vendor requests with bRequest = 0xB0, 0xB3, 0xB7, 0xB9
(codes between valid commands that have no handler). Expect STALL (PIPE error)
for each. Follow with TESTFX3 alive check.

**Why**: Current `raw` test only covers 0xB4/0xB5/0xB8 (removed R82xx cmds).
Other dead-zone codes exercise the same dispatch default path but confirm no
accidental match.

**Soak**: Yes, weight 3 (lightweight bounds check).

---

### T3 — `setarg_gap_index` (near-miss wIndex values for SETARGFX3)

**What**: Send SETARGFX3 with wIndex = 12, 13, 15 (gaps between valid
arg IDs 10/11/14). Expect STALL for each. TESTFX3 alive check after.

**Why**: `oob_setarg` only tests wIndex=0xFFFF (way out of range). Near-miss
values catch off-by-one errors in the switch/case or lookup table.

**Soak**: Yes, weight 3 (lightweight).

---

### T4 — `gpio_extremes` (edge GPIO patterns)

**What**: Send GPIOFX3 with values 0x00000000, 0xFFFFFFFF, 0x0001FFFF (all
valid GPIO pins asserted). TESTFX3 alive check after each.

**Why**: Current tests use specific LED patterns. Extreme values exercise
bit-masking logic and confirm no crash on all-ones (bits above pin count).

**Soak**: No (deterministic, fast).

---

### T5 — `dma_count_reset` (counter semantics across sessions)

**What**: STARTADC(64M) → STARTFX3 → bulk_read 64 KB → STOPFX3 → GETSTATS
(expect `dma_completions > 0`) → STARTFX3 → STOPFX3 immediately → GETSTATS
(expect `dma_completions` reset to 0 or near-zero).

**Why**: No test verifies that `glDMACount` resets on each STARTFX3.
A stale count could mislead diagnostics.

**Soak**: Yes, weight 5 (state management check).

---

### T6 — `dma_count_monotonic` (counter grows during stream)

**What**: STARTADC(64M) → STARTFX3 → 10 iterations of
(bulk_read 32 KB + GETSTATS → record `dma_completions`) → STOPFX3.
Assert each successive `dma_completions` value is strictly greater than
the previous.

**Why**: Confirms GETSTATS reflects live DMA activity and the counter doesn't
stall or regress.

**Soak**: Yes, weight 5 (light, good health indicator).

---

### T7 — `watchdog_cap_observe` (cap plateau verification)

**What**: Set WDG_MAX_RECOV=3 → STARTADC(64M) → STARTFX3 → **do not read** →
poll GETSTATS every 500 ms for 10 seconds → STOPFX3. Assert
`streaming_faults` climbs to exactly 3 and then plateaus (subsequent polls
show the same value).

**Why**: `abandoned_stream` exercises this path but only checks the final
counter. This test validates the cap *mechanism* by observing the plateau
in real time.

**Soak**: Yes, weight 5 (watchdog path).

---

### T8 — `watchdog_cap_restart` (restart after cap)

**What**: Same setup as T7, but after the plateau: (without STOP) issue
STARTFX3 → bulk_read ≥ 1 KB → STOPFX3. PASS if data received.

**Why**: Verifies the firmware can cleanly restart streaming after the
watchdog has given up, without an intervening STOPFX3.

**Soak**: Yes, weight 5 (recovery path).

---

### T9 — `i2c_write_bad_addr` (I2C write to absent device)

**What**: GETSTATS (baseline `i2c_failures`) → I2CWFX3 to address 0x90
(absent), register 0, data byte 0x00 → TESTFX3 alive → GETSTATS →
assert `i2c_failures` incremented by 1.

**Why**: `i2c_bad_addr` only tests I2C *read* to an absent address.
A write NACK follows a different firmware code path (CyU3PI2cTransmitBytes
vs CyU3PI2cReceiveBytes).

**Soak**: Yes, weight 3 (quick error-path check).

---

### T10 — `i2c_multibyte` (multi-byte I2C round-trip)

**What**: I2CRFX3 read 8 bytes from Si5351 starting at reg 0 → save →
I2CWFX3 write those same 8 bytes back → I2CRFX3 read 8 bytes again →
compare. PASS if identical.

**Why**: All existing I2C tests use single-byte transfers. Multi-byte
exercises the EP0 data-phase sizing and the I2C burst path.

**Soak**: Yes, weight 3 (I2C path).

---

### T11 — `ep0_hammer` (EP0 saturation during stream)

**What**: STARTADC(64M) → STARTFX3 → 500 × rapid TESTFX3 (no sleep) →
bulk_read ≥ 1 KB → STOPFX3. PASS if bulk data still arrives after the
EP0 burst.

**Why**: `ep0_control_while_streaming` sends only 20 EP0 commands. 500
back-to-back commands check whether EP0 dispatch can starve the DMA path.

**Soak**: Yes, weight 3 (stress).

---

### T12 — `debug_cmd_while_stream` (debug command during active stream)

**What**: STARTADC(64M) → STARTFX3 → enable debug (TESTFX3 wValue=1) →
send "gpif" command via READINFODEBUG → poll READINFODEBUG for response →
bulk_read ≥ 1 KB → STOPFX3. PASS if both debug output and bulk data
received.

**Why**: `debug_while_stream` polls output but never sends a command.
Command dispatch on the app thread contests with the watchdog timer —
this test exercises that contention.

**Soak**: Yes, weight 3 (concurrency).

---

### T13 — `adc_freq_extremes` (edge frequencies)

**What**: STARTADC at 1 000 000 Hz (1 MHz), then 200 000 000 Hz (200 MHz),
then 1 Hz. After each: GETSTATS → check PLL status → TESTFX3 alive.
Restore normal frequency at end.

**Why**: All current tests use standard frequencies (16–128 MHz). Edge
frequencies stress the Si5351 PLL divider computation and may reveal
divide-by-zero or overflow in the firmware's frequency math.

**Soak**: No (deterministic, restores clock state).

---

### T14 — `readinfodebug_flood` (debug buffer overrun without drain)

**What**: Enable debug (TESTFX3 wValue=1) → 50 × SETARGFX3 (rapid, generates
TraceSerial output) with **no** READINFODEBUG between them → single
READINFODEBUG read → TESTFX3 alive.

**Why**: Current `debug_race` interleaves writes and reads. This test
fills the 256-byte `glBufDebug` without draining and verifies the
circular buffer wraps without corruption or crash.

**Soak**: Yes, weight 3 (buffer stress).

---

### T15 — `data_sanity` (basic data integrity)

**What**: Set attenuator to maximum (63), VGA to minimum (0), SHDWN GPIO
asserted → STARTADC(64M) → STARTFX3 → capture 1 MB → STOPFX3. Parse
captured samples as 16-bit signed integers. Assert no sample equals
+32767 or −32768 (full-scale saturation).

**Why**: No test inspects bulk data content at all. With the analog
front-end shut down, full-scale samples indicate DMA corruption (stale
descriptors, buffer reuse, etc.), not real signal.

**Implementation note**: This is the most complex new test — it requires
parsing the raw sample buffer. If the SHDWN GPIO is not available on
all hardware variants, degrade to a skip.

**Soak**: Yes, weight 2 (slow, hardware-dependent).

---

## Changes to fw_test.sh (TAP suite)

Add the following tests to the automated TAP suite. These are the subset
of the above that are deterministic, fast, and don't require streaming:

| New TAP # | fx3_cmd subcommand | Description |
|-----------|--------------------|-------------|
| 31 | `vendor_rqt_wrap` | Counter wraparound at 256 |
| 32 | `stale_vendor_codes` | Dead-zone bRequest values STALL |
| 33 | `setarg_gap_index` | Near-miss wIndex values STALL |
| 34 | `gpio_extremes` | All-zeros / all-ones GPIO patterns |
| 35 | `i2c_write_bad_addr` | I2C write NACK counter |

Increment `PLANNED` from 30 to 35 (line 181 of fw_test.sh).

The streaming-dependent tests (T5–T8, T11–T12, T15) are better suited
to the soak harness and should not be in the quick TAP run.

---

## Changes to soak scenario table

Add these entries to `scenarios[]` (fx3_cmd.c line 2913):

| Scenario | Function | Weight |
|----------|----------|--------|
| `stale_vendor_codes` | `do_test_stale_vendor_codes` | 3 |
| `setarg_gap_index` | `do_test_setarg_gap_index` | 3 |
| `dma_count_reset` | `do_test_dma_count_reset` | 5 |
| `dma_count_monotonic` | `do_test_dma_count_monotonic` | 5 |
| `watchdog_cap_observe` | `do_test_watchdog_cap_observe` | 5 |
| `watchdog_cap_restart` | `do_test_watchdog_cap_restart` | 5 |
| `i2c_write_bad_addr` | `do_test_i2c_write_bad_addr` | 3 |
| `i2c_multibyte` | `do_test_i2c_multibyte` | 3 |
| `ep0_hammer` | `do_test_ep0_hammer` | 3 |
| `debug_cmd_while_stream` | `do_test_debug_cmd_while_stream` | 3 |
| `readinfodebug_flood` | `do_test_readinfodebug_flood` | 3 |
| `data_sanity` | `do_test_data_sanity` | 2 |

Total new soak weight: +43 (current total ≈ 212, new total ≈ 255).

---

## Implementation order

1. **T1–T4**: Pure EP0, no streaming, simplest to implement and verify.
2. **T9–T10**: I2C tests, no streaming, moderate complexity.
3. **T14**: Debug buffer, no streaming.
4. **T5–T8**: DMA/watchdog, require streaming and timed waits.
5. **T11–T12**: Stress/concurrency, require streaming.
6. **T13**: Frequency extremes (may behave differently per hardware).
7. **T15**: Data sanity (most complex, hardware-dependent).
8. **fw_test.sh updates**: Add TAP entries for T1–T4, T9.
9. **Soak table updates**: Add all soak-eligible tests.

---

## Files modified

| File | Changes |
|------|---------|
| `tests/fx3_cmd.c` | 15 new `do_test_*()` functions, 15 forward declarations, 15 `local_cmds_noarg[]` entries, 12 `scenarios[]` entries |
| `tests/fw_test.sh` | 5 new TAP tests (T1–T4, T9), `PLANNED` bump 30→35 |

No new files. No build system changes (fx3_cmd.c is a single-file compile).

---

## Validation

After implementation, run:

```bash
# Build
cd tests && make clean && make

# Quick TAP (should show 1..35, all pass)
./fw_test.sh

# Soak (15 minutes, confirm new scenarios appear in summary)
./soak_test.sh --hours 0.25
```
