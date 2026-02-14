#!/bin/bash
# Create GitHub issues for post-merge cleanup and documentation tasks.
# Run once from a machine with `gh` CLI authenticated.
# Usage: bash scripts/create-cleanup-issues.sh

set -euo pipefail

REPO="ringof/ExtIO_sddc"
LABEL_CLEANUP="cleanup"
LABEL_DOCS="documentation"

# Ensure labels exist (ignore error if they already do)
gh label create "$LABEL_CLEANUP" --repo "$REPO" \
   --description "Code quality / cleanup tasks" \
   --color "c5def5" 2>/dev/null || true
gh label create "$LABEL_DOCS" --repo "$REPO" \
   --description "Documentation updates" \
   --color "0075ca" 2>/dev/null || true

create_issue() {
    local title="$1"
    local body="$2"
    local label="$3"
    if gh issue list --repo "$REPO" --search "\"$title\" in:title" --json number -q '.[0].number' | grep -q .; then
        echo "SKIP (exists): $title"
    else
        gh issue create --repo "$REPO" --title "$title" --body "$body" --label "$label"
        echo "CREATED: $title"
    fi
}

# ── Code cleanup issues (items 11-16) ──

create_issue \
  "Consolidate watchdog DebugPrint calls in RunApplication.c" \
  "$(cat <<'EOF'
## Problem

The watchdog recovery path in `RunApplication.c` (lines ~241-273) has 9 `DebugPrint` calls. Since `DebugPrint2USB` shares a 256-byte buffer, this many prints in a single execution path risks filling the buffer and dropping messages — especially under the conditions (DMA stall) where recovery diagnostics are most valuable.

## Proposed Fix

Consolidate to 2-3 essential prints:
1. `WDG: === RECOVERY START === SM=%d DMA=%d` (entry)
2. `WDG: === RECOVERY %s === rc=%d` (result: OK or FAIL + first non-zero return code)

Remove the per-step prints (`WDG: DmaReset rc=%d`, `WDG: FlushEp rc=%d`, etc.) that were useful during development.

## Files
- `SDDC_FX3/RunApplication.c`
EOF
)" "$LABEL_CLEANUP"

create_issue \
  "Remove redundant braces from deleted loop in RunApplication.c" \
  "$(cat <<'EOF'
## Problem

In `RunApplication.c` around line 206, there are braces `{...}` wrapping the event-processing block that were left behind when the `for (Count = 0; Count<10; Count++)` loop was removed. They add a misleading scope level.

## Proposed Fix

Remove the outer `{` and `}` and dedent the enclosed code.

## Files
- `SDDC_FX3/RunApplication.c`
EOF
)" "$LABEL_CLEANUP"

create_issue \
  "Rename GETSTATS 'ep_underruns' field to 'streaming_faults'" \
  "$(cat <<'EOF'
## Problem

`glCounter[2]` is incremented by both:
- The EP_UNDERRUN DMA callback (genuine endpoint underruns)
- The GPIF watchdog recovery path (DMA stall recovery)

The GETSTATS response and `fx3_cmd` display label it `ep_underruns` / `underrun`, which is misleading when watchdog recoveries are also counted.

## Options

1. **Rename only:** Change the label to `streaming_faults` to encompass both meanings (minimal firmware change)
2. **Separate counters:** Use `glCounter[3]` for watchdog recoveries, report as a separate GETSTATS field (requires protocol change + host tool update)

Option 1 is simpler; option 2 gives better diagnostics.

## Files
- `SDDC_FX3/USBHandler.c` (GETSTATS packing)
- `SDDC_FX3/RunApplication.c` (watchdog increment)
- `tests/fx3_cmd.c` (display label)
- `SDDC_FX3/docs/debugging.md` (GETSTATS table)
EOF
)" "$LABEL_CLEANUP"

create_issue \
  "Add ConfGPIOSimple() declaration to Application.h" \
  "$(cat <<'EOF'
## Problem

`ConfGPIOSimple()` is the unified GPIO configuration function in `RunApplication.c`, replacing the legacy `ConfGPIOsimpleout()` / `ConfGPIOsimpleinput()` wrappers. However, `ConfGPIOSimple()` is not declared in `Application.h` — only the legacy wrappers are.

## Proposed Fix

Add `ConfGPIOSimple()` declaration to `Application.h`. Consider whether the legacy wrappers can be removed if all call sites use the unified function.

## Files
- `SDDC_FX3/Application.h`
- `SDDC_FX3/RunApplication.c`
EOF
)" "$LABEL_CLEANUP"

create_issue \
  "Add comment for stale tuner defines in fx3_cmd.c" \
  "$(cat <<'EOF'
## Problem

`tests/fx3_cmd.c` lines 47-50 define `TUNERINIT`, `TUNERTUNE`, `TUNERSTDBY` — commands for the removed R82xx tuner driver. They exist solely for the stale-command regression tests (which verify the firmware correctly STALLs on removed vendor requests), but this is not documented.

## Proposed Fix

Add a comment above the defines:
```c
/* Removed vendor commands — kept for stale-command regression tests
 * that verify the firmware correctly rejects them with STALL. */
```

## Files
- `tests/fx3_cmd.c`
EOF
)" "$LABEL_CLEANUP"

create_issue \
  "Handle unreachable tcsetattr in do_debug() (fx3_cmd.c)" \
  "$(cat <<'EOF'
## Problem

In `tests/fx3_cmd.c`, `do_debug()` has a `for (;;)` loop with no `break` path. The `tcsetattr(STDIN_FILENO, TCSANOW, &oldt)` and `return 0` after the loop are unreachable dead code. When the user presses Ctrl-C, the terminal is left in raw mode.

## Proposed Fix

Add a `SIGINT` handler that:
1. Restores the terminal to the saved `oldt` settings
2. Exits cleanly (or breaks the loop)

Alternatively, use `atexit()` to register terminal restoration.

## Files
- `tests/fx3_cmd.c`
EOF
)" "$LABEL_CLEANUP"

# ── Documentation issues ──

create_issue \
  "docs: Update debugging.md buffer size and drain behavior" \
  "$(cat <<'EOF'
## Problem

`SDDC_FX3/docs/debugging.md` has two stale values:

1. **Buffer size:** Doc says `glBufDebug[100]` and `MAXLEN_D_USB` = 100. Actual size is now 256 (`protocol.h` line 71).
2. **READINFODEBUG drain:** Doc says "up to 100 bytes of ASCII" per transfer. Actual implementation clamps to 63 bytes (`CYFX_SDRAPP_MAX_EP0LEN - 1`) per EP0 transfer, with `memmove` to preserve the remainder for the next poll.

## Files
- `SDDC_FX3/docs/debugging.md` — sections 1.3 and "Output Buffer" table
- Ground truth: `SDDC_FX3/protocol.h:71`, `SDDC_FX3/USBHandler.c:355-368`
EOF
)" "$LABEL_DOCS"

create_issue \
  "docs: Add GPIF watchdog section to debugging.md" \
  "$(cat <<'EOF'
## Problem

`SDDC_FX3/docs/debugging.md` does not mention the GPIF watchdog recovery mechanism. The watchdog is a debug-observable feature: it logs `WDG:` prefixed messages to the debug console and increments `glCounter[2]` (visible in GETSTATS).

## What to document

- Detection criteria: DMA count stalled + GPIF in BUSY/WAIT state (5, 7, 8, 9) for 3 consecutive 100ms polls
- Recovery actions: GpifDisable, DMA reset, EP flush, PLL check, optional auto-restart
- Debug log message format (`WDG: stall N/3`, `WDG: === RECOVERY START/DONE ===`)
- Counter: `glCounter[2]` at GETSTATS offset 15-18

## Files
- `SDDC_FX3/docs/debugging.md`
- Ground truth: `SDDC_FX3/RunApplication.c:216-295`
EOF
)" "$LABEL_DOCS"

create_issue \
  "docs: Add missing fx3_cmd test commands to debugging.md" \
  "$(cat <<'EOF'
## Problem

The fx3_cmd command table in `SDDC_FX3/docs/debugging.md` section 6.1 is missing five test commands:

| Command | Implementation |
|---------|---------------|
| `stats_pll` | `fx3_cmd.c:do_test_stats_pll()` |
| `stop_gpif_state` | `fx3_cmd.c:do_test_stop_gpif_state()` |
| `stop_start_cycle` | `fx3_cmd.c:do_test_stop_start_cycle()` |
| `pll_preflight` | `fx3_cmd.c:do_test_pll_preflight()` |
| `wedge_recovery` | `fx3_cmd.c:do_test_wedge_recovery()` |

These are also available as local commands via `!` prefix in debug mode.

## Files
- `SDDC_FX3/docs/debugging.md` — section 6.1
- Ground truth: `tests/fx3_cmd.c:1634-1645`
EOF
)" "$LABEL_DOCS"

create_issue \
  "docs: Update architecture.md with GPIF preflight and watchdog" \
  "$(cat <<'EOF'
## Problem

`docs/architecture.md` does not document two major firmware features:

1. **GPIF preflight check** (`GpifPreflightCheck` in `StartStopApplication.c:67-100`): STARTFX3 verifies `si5351_clk0_enabled()` and `si5351_pll_locked()` before starting the GPIF. If either check fails, STARTFX3 is rejected with STALL.

2. **GPIF watchdog** (`RunApplication.c:216-295`): Monitors DMA throughput, detects stalls when GPIF is in BUSY/WAIT state, performs automatic recovery after 300ms.

Additionally, the STARTFX3/STOPFX3 lifecycle descriptions in the command table are stale — they don't reflect the force-disable-before-restart pattern, waveform reload, or LPM management.

## Files
- `docs/architecture.md` — command table and "Software control of GPIF" section
- Ground truth: `SDDC_FX3/USBHandler.c:272-319`, `SDDC_FX3/StartStopApplication.c:67-100`, `SDDC_FX3/RunApplication.c:216-295`
EOF
)" "$LABEL_DOCS"

create_issue \
  "docs: Fix architecture.md SETARGFX3 and GPIO bitmask errors" \
  "$(cat <<'EOF'
## Problem

Two factual errors in `docs/architecture.md`:

1. **SETARGFX3 "protocol oddity" paragraph is stale:** The doc says the default case leaves `isHandled` false, causing a double-stall. The code now sets `isHandled = CyTrue` after the stall (`USBHandler.c:263-268`), so only a single stall occurs.

2. **LED_BLUE GPIO bitmask:** The doc lists LED_BLUE at bit 12. The actual definition is `LED_BLUE = OUTXI11` (bit 11) in `protocol.h:53`.

## Files
- `docs/architecture.md` — "Error handling" section and GPIO bitmask table
- Ground truth: `SDDC_FX3/USBHandler.c:263-268`, `SDDC_FX3/protocol.h:53`
EOF
)" "$LABEL_DOCS"

create_issue \
  "docs: Update wedge_detection.md from proposal to implementation" \
  "$(cat <<'EOF'
## Problem

`docs/wedge_detection.md` was written as a design proposal before the watchdog was implemented. It contains multiple stale statements:

- Line 55: "The firmware has no watchdog, no throughput monitor" — now false
- Implementation priorities table (lines 266-274) does not reflect that priorities 1-5 are all complete
- Recovery strategy section describes a proposal rather than the actual implementation
- PIB callback line number reference (129) is stale — actual is now line 167

## Proposed Fix

Update the document to describe the implemented system:
- Mark completed priorities in the table
- Replace "proposed" language with "implemented" descriptions
- Document the actual watchdog algorithm: 3-poll threshold, GPIF states 5/7/8/9, DMA count > 0 guard, auto-restart with PLL check
- Fix stale line number references

## Files
- `docs/wedge_detection.md`
- Ground truth: `SDDC_FX3/RunApplication.c:216-295`, `SDDC_FX3/StartStopApplication.c:67-100,167`
EOF
)" "$LABEL_DOCS"

create_issue \
  "docs: Add tests/README.md" \
  "$(cat <<'EOF'
## Problem

The `tests/` directory has no README or standalone documentation file. Build instructions, prerequisites, and usage are scattered across:
- `fx3_cmd.c` line 13 (inline build command)
- `tests/Makefile` (make targets)
- `docs/architecture.md` lines 750-754
- `SDDC_FX3/docs/debugging.md` lines 307-309

## Proposed Content

A `tests/README.md` covering:
1. Prerequisites (`libusb-1.0-0-dev`, submodule init for `rx888_tools`)
2. Build: `make` (or `make fx3_cmd`)
3. Running `fx3_cmd` subcommands (test, debug, stats, etc.)
4. Running `fw_test.sh` (TAP output, device requirements)
5. Brief description of the local command escape (`!`) in debug mode

## Files
- New: `tests/README.md`
- Reference: `tests/Makefile`, `tests/fx3_cmd.c`, `tests/fw_test.sh`
EOF
)" "$LABEL_DOCS"

echo ""
echo "Done. Created 13 issues (6 cleanup + 7 documentation)."
