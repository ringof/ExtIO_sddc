#!/usr/bin/env bash
#
# create-soak-issues.sh — Create GitHub issues for the two soak test
# failure populations identified during multi-hour stress testing.
#
# Run once locally:  bash create-soak-issues.sh
# Requires: gh CLI authenticated with push access to ringof/ExtIO_sddc
#

set -euo pipefail

REPO="ringof/ExtIO_sddc"

echo "Creating issues in $REPO..."

# ---- Issue 1: Population A — 0x1005 "dead on first start" ----

gh issue create --repo "$REPO" \
  --title "Soak: STARTFX3 occasionally fails to produce data (0x1005, Population A)" \
  --label "bug" \
  --body "$(cat <<'ISSUE_A_EOF'
## Summary

During multi-hour soak testing (~10% of scenarios), the first bulk
transfer after STARTFX3 returns 0 bytes with PIB error code 0x1005.
The stream never starts — `dma_completions=0`, GPIF state reads 255
(disabled) at failure time, and the Si5351 PLL is locked (0x01).

## Failure signature

| Field | Value |
|-------|-------|
| pib_arg | 0x1005 |
| dma_count | 0 (always) |
| gpif_state | 255 (always) |
| si5351 | 0x01 (PLL locked) |
| pib_errors | 3–6 per failed start (varies with timing) |
| faults | 0–1 (not correlated) |

## Observed predecessors (not correlated — any scenario can trigger)

- watchdog_cap_observe, watchdog_cap_restart, wedge_recovery
- ep0_stall_recovery, i2c_multibyte, pll_preflight
- (none / first scenario in chunk after firmware reload)

## Affected scenarios (any that attempt streaming)

sustained_stream, clock_pull, freq_hop, rapid_start_stop,
watchdog_cap_restart, wedge_recovery, dma_count_reset

## Hypothesis

The STARTFX3 handler in USBHandler.c arms DMA (`SetXfer`), loads and
starts GPIF (`GpifLoad` + `GpifSMStart`), then asserts FW_TRG
(`GpifControlSWInput(CyTrue)`).  There is no settling delay between
these steps.  If the GPIF state machine begins clocking before the
DMA thread sockets are fully armed, the first GPIF writes generate
PIB errors (0x1005 = write to unready socket) and the pipeline is
dead on arrival.

The host-side `fx3_cmd` test harness uses synchronous `bulk_read_some()`
for most scenarios (submit TD *after* STARTFX3), creating a window
where 4× 16 KB DMA buffers fill before the host has queued any
transfers.  `rx888_stream` avoids this by pre-submitting 32 async
transfers BEFORE issuing STARTFX3.  The existing `primed_start_and_read()`
helper in fx3_cmd.c implements this pattern but is only used by a few
scenarios.

## Investigation plan

1. Add firmware-side instrumentation: capture DMA SetXfer return code
   and GPIF SM state immediately after each step in STARTFX3.
2. Add a small delay (`CyU3PThreadSleep(1)`) between SetXfer and
   StartGPIF to test the timing hypothesis.
3. Audit all soak scenarios to identify which use synchronous
   `bulk_read_some()` after STARTFX3 vs primed async reads.
4. Compare failure rates with and without the firmware delay.

## References

- `SDDC_FX3/USBHandler.c` lines 319–373 (STARTFX3 handler)
- `tests/fx3_cmd.c` lines 1433–1519 (primed_start_and_read)
- `tests/fx3_cmd.c` lines 1383–1393 (bulk_read_some)
ISSUE_A_EOF
)"

echo "  Issue 1 (Population A) created."

# ---- Issue 2: Population B — 0x1006 dma_count_reset-specific ----

gh issue create --repo "$REPO" \
  --title "Soak: dma_count_reset always fails with 0x1006 (Population B)" \
  --label "bug" \
  --body "$(cat <<'ISSUE_B_EOF'
## Summary

The `dma_count_reset` soak scenario fails with PIB error code 0x1006
("invalid GPIF state") instead of the 0x1005 seen in Population A.
This error is **exclusive to dma_count_reset** — 5 out of 5 observed
0x1006 failures came from this single scenario, with 5 different
predecessors.  The failure cascades: once it occurs, subsequent
scenarios in the same soak chunk accumulate escalating PIB error
counts.

## Failure signature

| Field | Value |
|-------|-------|
| pib_arg | 0x1006 |
| dma_count | 0 (always) |
| gpif_state | 255 (always) |
| si5351 | 0x01 (PLL locked) |
| pib_errors | 7–137 (escalates across cascading failures) |
| faults | 2–4 (escalates) |

## Observed predecessors (not correlated)

freq_hop, pll_preflight, dma_count_reset (itself), wedge_recovery,
stop_start_cycle

## What makes dma_count_reset unique

The scenario has a **two-session structure**:

1. STARTADC(64 MHz) → STARTFX3 → `bulk_read_some(64 KB)` → STOPFX3 → GETSTATS
2. sleep 200 ms
3. STARTFX3 → STOPFX3 (immediately, no read) → GETSTATS

Session 2's "start then immediately stop without reading" is unique
among the 39 soak scenarios.  When session 1 fails (count1=0), the
test still proceeds to session 2, compounding the damage.

## Hypothesis

The "start then immediate stop" in session 2 triggers a GPIF state
machine transition that `CyU3PGpifDisable(CyTrue)` does not fully
clean up.  Specifically:

- STARTFX3 calls `GpifLoad()` + `GpifSMStart()` + `GpifControlSWInput(CyTrue)`
- STOPFX3 arrives before any DMA completion occurs
- The GPIF SM is mid-transition (waveform active, FW_TRG asserted,
  but no data has flowed through the DMA channel)
- `GpifDisable(CyTrue)` force-stops the SM but leaves internal GPIF
  registers (e.g., counter/comparator state) in an inconsistent state
- The next STARTFX3's `GpifLoad()` + `GpifSMStart()` inherits this
  corruption → 0x1006

The escalation pattern (pib 7→13→137 within one chunk) confirms
the corruption is cumulative and not cleared by the inter-scenario
STOPFX3 + 100ms sleep in the soak harness.

## Investigation plan

1. Add firmware debug output: print GPIF SM state before and after
   each step of STARTFX3 and STOPFX3.
2. Test whether adding a `CyU3PGpifLoad()` (waveform reload) in
   STOPFX3 after `GpifDisable` clears the residual state.
3. Modify `dma_count_reset` session 2 to add a short delay (e.g.,
   10ms) between STARTFX3 and STOPFX3 and check if 0x1006 rate drops.
4. Modify `dma_count_reset` to bail out early when session 1 fails
   (count1=0) instead of proceeding to session 2.

## References

- `tests/fx3_cmd.c` lines 3138–3206 (dma_count_reset scenario)
- `SDDC_FX3/USBHandler.c` lines 319–391 (STARTFX3/STOPFX3 handlers)
- `SDDC_FX3/StartStopApplication.c` lines 112–121 (StartGPIF)
ISSUE_B_EOF
)"

echo "  Issue 2 (Population B) created."
echo "Done. Both issues created."
