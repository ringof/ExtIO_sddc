# PLAN: Soak Test Failure Populations A & B

## Background

Multi-hour soak testing (39 scenarios, 20 cycles per chunk, firmware
reload between chunks) reveals two distinct failure populations, both
resulting in "stream produces 0 bytes":

- **Population A (0x1005):** ~10% per-cycle rate, any streaming scenario,
  predecessor-independent.  "Dead on first start."
- **Population B (0x1006):** Exclusive to `dma_count_reset`, cascading.
  "Invalid GPIF state from start-then-immediate-stop."

---

## Population A: "Dead on first start" (0x1005)

### Observed data (7 samples)

| Scenario | Predecessor | pib | faults |
|----------|-------------|-----|--------|
| watchdog_cap_restart | (self) | 4 | 1 |
| wedge_recovery | ? | 4 | 1 |
| sustained_stream | watchdog_cap_observe | 4 | 1 |
| clock_pull | ? (chunk start) | 3 | 0 |
| clock_pull | ep0_stall_recovery | 3 | 0 |
| freq_hop | ? | 3 | 0 |
| rapid_start_stop | i2c_multibyte | 105 | 0 |

### Root cause hypotheses

**H1 — GPIF starts before DMA is armed (firmware-side race):**
The STARTFX3 handler calls `SetXfer` → `StartGPIF` → `GpifControlSWInput`
with no settling delay.  If the GPIF SM begins clocking before the DMA
thread sockets complete their setup, the first writes generate 0x1005
errors.

**H2 — Host TD not queued before DMA buffers fill (host-side race):**
Most soak scenarios use synchronous `bulk_read_some()` after STARTFX3.
At 64 MS/s the 4×16 KB DMA buffers fill in ~500 µs.  The host-side
`cmd_u32(STARTFX3)` → `bulk_read_some()` gap is several milliseconds.
rx888_stream avoids this by pre-submitting 32 async transfers BEFORE
STARTFX3.  fx3_cmd's `primed_start_and_read()` does the same but is
only used by a few scenarios.

**H3 — USB endpoint ring stale after previous scenario:**
After STOPFX3, the xHCI endpoint ring may retain state from the
previous transfer.  The inter-scenario 100 ms sleep may not be enough
for the xHCI to fully reset.

### Debugging plan

#### Step 1: Instrument firmware STARTFX3 (proves H1)

Add debug prints capturing the return code and timing of each step:

```c
// In STARTFX3 handler (USBHandler.c ~line 357):
DebugPrint(4, "\r\nSTART: xfer=%d", apiRetStatus);
// After StartGPIF():
{ uint8_t _t=0xFF; CyU3PGpifGetSMState(&_t);
  DebugPrint(4, " gpif=%d sm=%d", apiRetStatus, _t); }
// After GpifControlSWInput:
DebugPrint(4, " trg=1");
```

Run soak with serial console captured.  Look for sequences where
`xfer=0` (success) but the first DMA callback never fires.

#### Step 2: Test firmware-side settling delay (tests H1)

Add `CyU3PThreadSleep(1)` (1 ms) between `SetXfer` and `StartGPIF`
in the STARTFX3 handler.  Run the same soak seed.  If Population A
disappears, H1 is confirmed.

#### Step 3: Migrate scenarios to primed reads (tests H2)

**Critical: respect the same timing as rx888_stream.**  The soak
harness opens the USB device with `open_rx888()` which calls
`libusb_clear_halt(EP1_IN)` at open time.  But unlike rx888_stream,
it does NOT pre-submit bulk transfers before STARTFX3.

For each scenario that currently does:
```c
cmd_u32(h, STARTFX3, 0);
bulk_read_some(h, N, timeout);
```

Replace with:
```c
primed_start_and_read(h, N, timeout);
```

Or for scenarios that need retry resilience:
```c
primed_start_and_read_retry(h, N, timeout);
```

This matches rx888_stream's approach of having the host ready to
receive before the firmware starts generating data.

**Scenarios to audit and migrate:**

| Scenario | Current approach | Migration |
|----------|-----------------|-----------|
| dma_count_reset (session 1) | cmd_u32_retry(STARTFX3) + bulk_read_some | primed_start_and_read_retry |
| clock_pull | cmd_u32(STARTFX3) + bulk_read_some | primed_start_and_read |
| freq_hop | cmd_u32(STARTFX3) + bulk_read_some | primed_start_and_read |
| sustained_stream | cmd_u32(STARTFX3) + bulk_read_some | primed_start_and_read |
| (others — audit needed) | TBD | TBD |

#### Step 4: Compare failure rates

Run identical soak seeds (same `--seed`, same `--reload-interval`)
with:
1. Baseline (current code)
2. Firmware delay only (Step 2)
3. Primed reads only (Step 3)
4. Both firmware delay and primed reads

This isolates whether the fix is firmware-side (H1), host-side (H2),
or both.

---

## Population B: dma_count_reset 0x1006 cascading failure

### Observed data (5 samples, all dma_count_reset)

| Predecessor | pib | faults | Cascading? |
|-------------|-----|--------|------------|
| freq_hop | 37 | 2 | unclear |
| pll_preflight | 7 | 3 | yes (row 1 of 3 in chunk) |
| dma_count_reset | 13 | 3 | yes (row 2 of 3) |
| stop_start_cycle | 137 | 4 | yes (row 3 of 3) |
| wedge_recovery | 46 | 2 | unclear |

### Root cause hypotheses

**H4 — Start-then-immediate-stop corrupts GPIF state:**
`dma_count_reset` session 2 sends STARTFX3 immediately followed by
STOPFX3 with no bulk read.  The GPIF SM starts (GpifLoad + SMStart
+ FW_TRG asserted) but is force-stopped before any DMA completion.
The SM was in a transitional state (actively trying to write to DMA
sockets that have no pending USB TDs).  `GpifDisable(CyTrue)` halts
the SM but may not reset internal GPIF counters, comparators, or
thread-to-socket mappings.  The next `GpifLoad`/`SMStart` inherits
this residue → 0x1006.

**H5 — Session 1 failure + session 2 compounds the damage:**
When session 1 already fails (count1=0, meaning Population A also hit),
session 2 proceeds anyway, adding another start/stop cycle on an
already-corrupted pipeline.  The test should bail early.

### Debugging plan

#### Step 5: Add early bailout to dma_count_reset

If `count1 == 0` (session 1 produced no data), skip session 2 and
return failure immediately.  This prevents the test from compounding
damage and isolates whether 0x1006 requires the session-2
start-then-immediate-stop or whether it can happen from session 1
alone.

#### Step 6: Instrument GPIF state through start/stop

In both STARTFX3 and STOPFX3, print GPIF SM state before and after
each operation:

```c
// STOPFX3 — after GpifDisable:
{ uint8_t _s=0xFF; CyU3PGpifGetSMState(&_s);
  DebugPrint(4, "\r\nSTP-post s=%d", _s); }
```

Capture serial console during a soak run that hits dma_count_reset.
Look for SM state values that indicate incomplete reset.

#### Step 7: Test delay between start and immediate stop

In `dma_count_reset` session 2, add `usleep(10000)` (10 ms) between
STARTFX3 and STOPFX3.  If 0x1006 disappears, the issue is the
SM being killed mid-transition.

#### Step 8: Test PIB clock re-init in STOPFX3

If the GPIF residue persists despite delays, try calling
`CyU3PPibDeInit()` + `CyU3PPibInit()` in STOPFX3 to fully reset
the PIB/GPIF block.  This is heavier but guarantees a clean slate.
Must verify this doesn't break the DMA channel binding.

---

## Execution order

| Priority | Step | Target | Effort |
|----------|------|--------|--------|
| 1 | Step 5 | Pop B — early bailout | 5 min (test-only change) |
| 2 | Step 1 | Pop A — firmware instrumentation | 15 min |
| 3 | Step 2 | Pop A — firmware settling delay | 5 min |
| 4 | Step 3 | Pop A — migrate to primed reads | 30 min (audit all scenarios) |
| 5 | Step 6 | Pop B — firmware instrumentation | 10 min |
| 6 | Step 7 | Pop B — delay in test | 5 min |
| 7 | Step 4 | Both — comparative soak runs | 2+ hours |
| 8 | Step 8 | Pop B — PIB re-init (if needed) | 30 min |

Steps 1–3 can be done in parallel with Steps 5–6.  Step 4 (validation
soak runs) requires firmware rebuild and a clean 2-hour soak window.

---

## Success criteria

- Population A: 0 failures in a 2-hour soak with `--reload-interval 20`
  (same seeds that previously triggered ~10% failure rate)
- Population B: 0 occurrences of 0x1006 across all scenarios
- No regression: overall pass rate ≥ current baseline for all other
  scenarios
