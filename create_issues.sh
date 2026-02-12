#!/usr/bin/env bash
# Create GitHub issues for SDDC_FX3 Phase 3: Cleanup & Consolidation
# Run once: bash create_issues.sh
# Requires: gh cli authenticated with repo access

set -euo pipefail

REPO="ringof/ExtIO_sddc"

echo "Creating Phase 3 cleanup issues for $REPO ..."

gh issue create --repo "$REPO" \
  --title "Extract GpioShiftOut helper to deduplicate bit-bang SPI in rx888r2.c" \
  --label "cleanup" \
  --body "$(cat <<'EOF'
## Problem

`rx888r2_SetAttenuator()` (lines 59-76) and `rx888r2_SetGain()` (lines 82-99)
in `SDDC_FX3/radio/rx888r2.c` contain identical bit-bang SPI loops differing
only in bit count (6 vs 8), mask, and latch pin.

## Proposed Change

- Add `static void GpioShiftOut(uint8_t latch_pin, uint8_t value, uint8_t bits)`
  containing the shared clock/data/latch loop
- Rewrite both functions as thin wrappers calling `GpioShiftOut`

## Files

- `SDDC_FX3/radio/rx888r2.c`

## Verification

`make clean && make all` must succeed.
EOF
)"
echo "  [1/8] GpioShiftOut helper"

gh issue create --repo "$REPO" \
  --title "Fix misleading and stale comments across 7 files" \
  --label "cleanup" \
  --body "$(cat <<'EOF'
## Problem

Several comments are wrong or stale after prior refactoring.

| Location | Problem | Fix |
|----------|---------|-----|
| `RunApplication.c:71,92` | ConfGPIO wrappers say "output" for input configs | Change to "input" / "input with pull-up" |
| `Support.c:179` | CheckStatusSilent says "displays and stall" | Rewrite to match actual blink-and-reset behavior |
| `i2cmodule.c:26` | Says "100KHz" bitrate | Change to "400KHz" to match `I2C_BITRATE` |
| `StartUP.c:44` | Says "GPIF can be '100MHz'" | Change to "~201MHz" (403MHz / clkDiv=2) |
| `Si5351.c:109` | References nonexistent "si5351a.h header file" | Remove or update reference |
| `docs/debugging.md:167` | MsgParsing label 2 says "free" | Update to describe PIB error info |
| `rx888r2.c:87` | SetGain comment says "ATT_LE latched" | Fix to "VGA_LE" |

## Verification

`make clean && make all` must succeed.
EOF
)"
echo "  [2/8] Misleading comments"

gh issue create --repo "$REPO" \
  --title "Remove dead commented-out code" \
  --label "cleanup" \
  --body "$(cat <<'EOF'
## Problem

Several files contain commented-out code and unused macros left over from
earlier development.

| Location | What to remove |
|----------|---------------|
| `StartStopApplication.c:45-49` | Commented-out `#define TH1_BUSY/TH1_WAIT/TH0_BUSY` block |
| `i2cmodule.h:27` | Commented-out `SendI2cbytes` declaration |
| `i2cmodule.h:9` | Unused `#define I2C_ACTIVE` macro |
| `i2cmodule.c:50-51` | Commented-out `DebugPrint` call |
| `RunApplication.c:224` | Commented-out `for (Count = 0; ...)` loop |
| `USBhandler.c:382` | Commented-out `DebugPrint` in LPMRequest_Callback |

## Verification

`make clean && make all` must succeed.
EOF
)"
echo "  [3/8] Dead code removal"

gh issue create --repo "$REPO" \
  --title "Rename inconsistent functions to PascalCase" \
  --label "cleanup" \
  --body "$(cat <<'EOF'
## Problem

Function naming is inconsistent across the codebase — mix of PascalCase,
camelCase, and underscore-separated styles.

## Proposed Renames

| Current name | New name | File(s) |
|-------------|----------|---------|
| `Pib_error_cb` | `PibErrorCallback` | StartStopApplication.c |
| `setupPLL` | `SetupPLL` | driver/Si5351.c (static) |
| `setupMultisynth` | `SetupMultisynth` | driver/Si5351.c (static) |
| `Si5351init` | `Si5351Init` | driver/Si5351.c, driver/Si5351.h, RunApplication.c |
| `USBEvent_Callback` | `USBEventCallback` | USBhandler.c, StartUP.c |
| `LPMRequest_Callback` | `LPMRequestCallback` | USBhandler.c, StartUP.c |

The `rx888r2_*` prefix is intentional namespacing for the radio driver — leave as-is.

## Verification

`make clean && make all` must succeed. Each rename should be a global
find-and-replace across all `.c` and `.h` files.
EOF
)"
echo "  [4/8] Function renames"

gh issue create --repo "$REPO" \
  --title "Move duplicate defines to protocol.h and fix circular include" \
  --label "cleanup" \
  --body "$(cat <<'EOF'
## Problem

`FX3_CMD_BASE`, `FX3_CMD_COUNT`, and `SETARGFX3_LIST_COUNT` are defined
identically in both `DebugConsole.c` and `USBhandler.c`. This violates
single-source-of-truth and risks divergence.

Additionally, `i2cmodule.h` includes `Application.h` which includes
`i2cmodule.h` — a circular dependency.

## Proposed Changes

1. Move all three defines into `protocol.h`
2. Remove duplicates from `DebugConsole.c` and `USBhandler.c`
3. Move `FX3CommandName[]` and `SETARGFX3List[]` arrays to a shared location
4. Fix `i2cmodule.h` to include only `cyu3types.h` instead of `Application.h`

## Files

- `protocol.h`, `DebugConsole.c`, `USBhandler.c`, `i2cmodule.h`

## Verification

`make clean && make all` must succeed.
EOF
)"
echo "  [5/8] Duplicate defines"

gh issue create --repo "$REPO" \
  --title "Consolidate duplicate GPIO LED setup in StartUP.c and Support.c" \
  --label "cleanup" \
  --body "$(cat <<'EOF'
## Problem

`IndicateError()` in `StartUP.c:17-33` and `ErrorBlinkAndReset()` in
`Support.c:27-50` both configure `GPIO_LED_BLUE_PIN` with identical
`CyU3PGpioSimpleConfig_t` initialization code.

## Proposed Change

Extract a shared `ConfigureLedGpio()` helper, or have `IndicateError()`
call `ErrorBlinkAndReset()` directly if the semantics align.

## Files

- `StartUP.c`, `Support.c`

## Verification

`make clean && make all` must succeed.
EOF
)"
echo "  [6/8] GPIO LED consolidation"

gh issue create --repo "$REPO" \
  --title "Rename files to consistent PascalCase" \
  --label "cleanup" \
  --body "$(cat <<'EOF'
## Problem

Most application source files use PascalCase (`RunApplication.c`,
`StartStopApplication.c`) but three files have inconsistent capitalization.

## Proposed Renames

| Current | Renamed |
|---------|---------|
| `USBdescriptor.c` | `USBDescriptor.c` |
| `USBhandler.c` | `USBHandler.c` |
| `StartUP.c` | `StartUp.c` |

## Changes Required

- `git mv` each file
- Update `SDDC_FX3/makefile` source lists
- Update any `#include` directives referencing these filenames

## Risk

Low — purely cosmetic. Git tracks renames cleanly.

## Verification

`make clean && make all` must succeed.
EOF
)"
echo "  [7/8] File renames"

gh issue create --repo "$REPO" \
  --title "Add gl prefix to unprefixed global variables" \
  --label "cleanup" \
  --body "$(cat <<'EOF'
## Problem

Many global variables lack the Cypress-convention `gl` prefix, making it
hard to distinguish globals from locals at a glance.

## Proposed Renames

| Current | Renamed |
|---------|---------|
| `vendorRqtCnt` | `glVendorRqtCnt` |
| `flagdebug` | `glFlagDebug` |
| `debtxtlen` | `glDebTxtLen` |
| `bufdebug` | `glBufDebug` |
| `ConsoleInBuffer` | `glConsoleInBuffer` |
| `ConsoleInIndex` | `glConsoleInIndex` |
| `ClockValue` | `glClockValue` |
| `Toggle` | `glToggle` |
| `HWconfig` | `glHWconfig` |
| `FWconfig` | `glFWconfig` |
| `BusSpeed` | `glBusSpeed` |
| `CyFxGpifName` | `glCyFxGpifName` |
| `EventAvailable` | `glEventAvailable` |
| `EventAvailableQueue` | `glEventAvailableQueue` |
| `Qevent` | `glQevent` |
| `ThreadHandle` | `glThreadHandle` |
| `StackPtr` | `glStackPtr` |

## Risk

Highest of all cleanup steps — touches `extern` declarations across many
files. Do one variable at a time, build-test between each.

## Verification

`make clean && make all` must succeed after each individual rename.
EOF
)"
echo "  [8/8] Global prefixes"

echo ""
echo "Done. All 8 issues created."
