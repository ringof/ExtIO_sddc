# Plan: Resume In-Band PPS — Re-apply Cross-Route Fix

## Context

Phase 4a testing (commit `19e2d40`) attempted the cross-route fix for the
per-commit `THR0_WR_OVERRUN` / `DATA_WRITE_ERR` that fires whenever the
SM reloads the just-committed thread before its socket finishes the
buffer switch.

The fix was reverted (`2273fb6`), but analysis of the diff reveals the
cross-route commit had a **second, unrelated bug**: one
`CY_U3P_PIB_GPIF_CTRL_BUS_SELECT` entry was deleted from
`CyFxGpifRegValue[]`, dropping the array from 76 to 75 entries. Since
`CyU3PGpifLoad` writes these positionally into the PIB GPIF registers,
every register from `CTRL_COUNT_CONFIG` onward was shifted by one —
corrupting `DATA_COUNT_LIMIT`, `CTRL_COMP_MASK`, all four
`THREAD_CONFIG` values, and `BETA_DEASSERT`.

### Confirmed severity (independently verified)

| Register | Should be | Actually written |
|---|---|---|
| DATA_COUNT_LIMIT | 0x00001FFE | wrong (shifted) |
| CTRL_COMP_MASK | 0x00000004 | wrong (shifted) |
| THREAD_CONFIG ×4 | 0x80010400..0403 | wrong (shifted) |
| BETA_DEASSERT | 0xFFFFFFC1 | garbage past end of array |

The `THREAD_CONFIG` corruption alone is sufficient to crash at boot —
those bind the four DMA threads to PIB sockets, and without them set
correctly the GPIF can't move data through any state, including just
sitting in IDLE. This fully explains the enumerate-then-disconnect
symptom observed in both Config A and Config B, with no "cross-thread
topology rejection" required.

Booting header (`2273fb6`): 16 `CTRL_BUS_SELECT` entries, **76** total
`CyFxGpifRegValue` entries.
Cross-route header (`19e2d40`): 15 entries, **75** total. One missing.

### WavedataPosition swap is correct

The WavedataPosition swap itself (`[12]=1,[13]=12` →
`[12]=12,[13]=1`) is correct: the COMMIT + IN_DATA actions are
encoded on the *inbound* transition from the EVENT states (unchanged by
the swap), and the PPS_COMMIT descriptor only determines where to go
*after* the commit. Descriptor 12 targets `TH1_RD_LD` and descriptor 1
targets `TH0_RD_LD`, which is exactly the cross-route intent. This
reading matches the descriptor-dedup pattern (TH0_PPS_COMMIT shares
descriptor 1 with IDLE — both are pass-through states whose only job is
the outbound transition).

The earlier "dedup creates an invalid cross-thread descriptor" hypothesis
was wrong. The actual cause was the one-line register array discrepancy.
The pps_inject overrun finding from before the cross-route still stands
as the original problem the cross-route was meant to fix.

## Steps

### 1. Fix the header (one-line restore)

In `SDDC_FX3/SDDC_GPIF_PPS.h`, apply the cross-route swap to
`CyFxGpifWavedataPosition[]` **without** deleting the register entry:

```c
uint8_t CyFxGpifWavedataPosition[]  = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,1       /* cross-route: TH0_PPS_COMMIT→TH1_RD_LD, TH1_PPS_COMMIT→TH0_RD_LD */
};
```

Leave `CyFxGpifRegValue[]` at its current 76-entry length (16
`CTRL_BUS_SELECT` entries). No other changes to the header. The
resulting diff against `2273fb6` should show **only** the
`CyFxGpifWavedataPosition` line changing — the register array must be
byte-for-byte identical. Restore the missing entry within the
`CTRL_BUS_SELECT` block (before `CTRL_COUNT_CONFIG`), not elsewhere.

### 2. Build verification

```
make -C SDDC_FX3 PPS_CTL_ENABLE=1 clean all    # Config B
make -C SDDC_FX3 clean all                      # Config A (regression)
make -C tests                                   # host tools
```

Confirm both images build clean and the Config B `.img` is the same size
(±8 bytes) as the current reverted build.

### 3. Hardware test — Phase 4a re-run

Flash Config B onto the dev board (with 100 kΩ GPIO 18↔19 loopback).

```
tests/fx3_cmd pps_inject
```

**Pass criteria** (binary, no middle ground):
- `pib_delta == 0` across the full rate ramp (1 → 100 Hz)
- No streaming faults, no device reset
- Throughput ≥ 95 % of baseline at every step

If this passes, the overruns were caused by the register shift, not by
the cross-route logic. Proceed to step 4.

If this still shows per-commit overruns at the same rates as before,
the register shift was masking (or compounding) a real cross-route
problem. See "Fallback" below.

### 4. Regression — Config A and Config B baseline

- Config A: full TAP suite (`tests/fw_test.sh`), confirm byte-for-byte
  production behavior.
- Config B with markers idle: full TAP suite + 1-hour soak, confirm no
  regression from the WavedataPosition change alone.

### 5. Phase 4a sign-off and commit

If steps 3–4 pass, commit the one-line fix with the evidence trail
(pps_inject output, soak log) and update the PR.

### 6. Resume Phase 4b+

Per `PLAN_INBAND_PPS_VALIDATION.md`:
- **Phase 4b**: Autonomous PPS driver (`synth_pps_commit_once()` body
  changes from `SetWrapUp` to GPIO 18 toggle) + marker-counting test
  gated to Config B.
- **Phase 5**: Host-side marker detection / sample-count integrity audit;
  real GPS PPS.

## Fallback

If the cross-route still overruns with the register array correct:

1. Check whether the overrun is on the **committed** thread (Thread 0)
   or the **destination** thread (Thread 1) — the `last_pib` error code
   will tell us.
2. If it's the destination thread, the issue is that the destination
   wasn't ready either (both threads had buffers filling). Mitigation:
   add a 1-cycle `DMA_RDY` wait state between `PPS_COMMIT` and
   `RD_LD`, at the cost of one dropped PCLK sample per marker.
3. If it's still the committed thread despite the cross-route, the
   DMA switch latency exceeds even the full-buffer settle time, and the
   architecture needs a different marker encoding (CPU-tagged sample
   count rather than short transfers).
