#!/usr/bin/env bash
#
# create-wedge-fix-issues.sh — Create GitHub issues for the GPIF wedge fix phases.
#
# Uses the local `gh` CLI.  Run once from the repo root:
#   ./scripts/create-wedge-fix-issues.sh
#
# Idempotent: skips creation if an issue with the same title already exists.

set -euo pipefail

REPO=$(gh repo view --json nameWithOwner -q .nameWithOwner 2>/dev/null || echo "")
if [[ -z "$REPO" ]]; then
    echo "error: not in a gh-authenticated repo" >&2
    exit 1
fi

create_issue() {
    local title="$1"
    local body="$2"
    local labels="${3:-}"

    # Check for existing issue with same title
    existing=$(gh issue list --search "\"$title\"" --state all --json title -q ".[].title" 2>/dev/null || echo "")
    if echo "$existing" | grep -qxF "$title"; then
        echo "SKIP: '$title' (already exists)"
        return
    fi

    local label_args=""
    if [[ -n "$labels" ]]; then
        label_args="--label $labels"
    fi

    gh issue create --title "$title" --body "$body" $label_args
    echo "CREATED: '$title'"
}

# ---- Phase 0 ----

create_issue \
    "GPIF wedge fix Phase 0: baseline test tooling" \
    "$(cat <<'BODY'
## Goal

Write test subcommands in `fx3_cmd` and entries in `fw_test.sh` that exercise
stop/start behavior.  Run them against the **current** firmware to confirm they
detect the broken behavior (all should FAIL).

## New `fx3_cmd` subcommands

- `stop_gpif_state` — STOP then GETSTATS, verify SM state is 0 or 1.
- `stop_start_cycle` — N iterations of START/stream/STOP, verify data each cycle.
- `pll_preflight` — turn off ADC clock, try START, verify rejection.
- `wedge_recovery` — provoke DMA backpressure, STOP+START, verify recovery.

## New `fw_test.sh` entries

4 new TAP tests inserted before `pib_overflow`.

## Test gate

Build `fx3_cmd`.  Run against current firmware:
- `stop_gpif_state`: expect FAIL
- `stop_start_cycle`: expect FAIL
- `pll_preflight`: expect FAIL
- `wedge_recovery`: expect FAIL

Reference: `PLAN-gpif-wedge-fix.md`
BODY
)" \
    "enhancement"

# ---- Phase 1 ----

create_issue \
    "GPIF wedge fix Phase 1: fix STOPFX3 (add GpifDisable)" \
    "$(cat <<'BODY'
## Goal

Make STOPFX3 actually stop the GPIF state machine.

## Change

**File:** `SDDC_FX3/USBHandler.c`, STOPFX3 case

- Add `CyU3PGpifDisable(CyFalse)` after `CyU3PGpifControlSWInput(CyFalse)`.
  This force-stops the SM even if stuck in BUSY/WAIT.  `CyFalse` keeps
  waveform memory loaded.
- Remove the 10 ms sleep — `GpifDisable` is synchronous.

## Test gate

Flash Phase 1 firmware:
- `stop_gpif_state`: **expect PASS** (SM state = 0 after STOP)
- `stop_start_cycle`: expect FAIL (START doesn't restart SM yet)
- All pre-existing `fw_test.sh` tests: expect PASS

Reference: `PLAN-gpif-wedge-fix.md`
BODY
)" \
    "bug"

# ---- Phase 2 ----

create_issue \
    "GPIF wedge fix Phase 2: fix STARTFX3 (add GpifSMStart)" \
    "$(cat <<'BODY'
## Goal

Make STARTFX3 restart the GPIF SM from a clean state after STOPFX3.

## Change

**File:** `SDDC_FX3/USBHandler.c`, STARTFX3 case

- Add `CyU3PGpifSMStart(0, 0)` to restart SM from state 0.
- Sequence: DMA reset → DMA xfer setup → SM start → SW input assert.
- Remove the leading `CyU3PGpifControlSWInput(CyFalse)` (SM already disabled).

## Test gate

Flash Phase 2 firmware:
- `stop_gpif_state`: **expect PASS**
- `stop_start_cycle`: **expect PASS** (key test — 5 cycles all stream data)
- `wedge_recovery`: **expect PASS**
- All pre-existing `fw_test.sh` tests: expect PASS
- Streaming tests: expect PASS

Reference: `PLAN-gpif-wedge-fix.md`
BODY
)" \
    "bug"

# ---- Phase 3 ----

create_issue \
    "GPIF wedge fix Phase 3: PLL lock pre-flight check in STARTFX3" \
    "$(cat <<'BODY'
## Goal

Refuse to start streaming if the ADC clock isn't running.

## Change

**File:** `SDDC_FX3/USBHandler.c`, STARTFX3 case (before DMA reset)

- Read Si5351 register 0, check bit 5 (LOL_A = PLL A loss-of-lock).
- If PLL unlocked, set `isHandled = CyFalse` and break.

## Test gate

Flash Phase 3 firmware:
- `pll_preflight`: **expect PASS** (START rejected without clock)
- `stop_start_cycle`: expect PASS (normal cycling unaffected)
- `wedge_recovery`: expect PASS
- All pre-existing `fw_test.sh` tests: expect PASS

Reference: `PLAN-gpif-wedge-fix.md`
BODY
)" \
    "enhancement"

# ---- Phase 4 ----

create_issue \
    "GPIF wedge fix Phase 4: GPIF watchdog in main loop" \
    "$(cat <<'BODY'
## Goal

Auto-detect and recover from a GPIF wedge without host intervention.

## Change

**File:** `SDDC_FX3/RunApplication.c`, ApplicationThread main loop

- Every 100 ms, compare `glDMACount` to previous value.
- If stalled and GPIF SM is in BUSY/WAIT (states 5, 7, 8, 9) for 3
  consecutive polls (300 ms), tear down and rebuild the streaming pipeline.
- Check Si5351 PLL lock to decide: auto-restart (PLL OK) or wait for
  host (PLL unlocked).
- Increment `glCounter[1]` on recovery for host visibility via GETSTATS.
- Add `extern` for `glMultiChHandleSlFifoPtoU`.

## Test gate

Flash Phase 4 firmware:
- `wedge_recovery`: **expect PASS** with `glCounter[1] > 0` (watchdog fired)
- `stop_start_cycle`: expect PASS
- `pll_preflight`: expect PASS
- `stop_gpif_state`: expect PASS
- All pre-existing `fw_test.sh` tests: expect PASS
- `pib_overflow`: should auto-recover, device responsive afterward

Reference: `PLAN-gpif-wedge-fix.md`
BODY
)" \
    "enhancement"

echo ""
echo "Done. All 5 phase issues created (or skipped if they already existed)."
