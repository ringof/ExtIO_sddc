#!/usr/bin/env bash
# create_issues.sh — Create GitHub issues for PLAN.md fixes #2, #8, #9
# Run once from the repo root.  Requires `gh` CLI authenticated.
# Idempotent: skips creation if an issue with the same title already exists.

set -euo pipefail

REPO="ringof/ExtIO_sddc"

create_issue() {
    local title="$1"
    local label="$2"
    local body="$3"

    # Skip if an open or closed issue with this exact title already exists
    if gh issue list --repo "$REPO" --search "\"$title\" in:title" --state all --limit 1 --json title -q '.[].title' \
        | grep -qxF "$title"; then
        echo "SKIP: issue already exists: $title"
        return
    fi

    gh issue create --repo "$REPO" \
        --title "$title" \
        --label "$label" \
        --body "$body"

    echo "CREATED: $title"
}

# ── Issue #1: Fix #2 — Debug NUL clobber + EP0 buffer overflow ───────────

create_issue \
    "READINFODEBUG clobbers last debug character and can overflow EP0 buffer" \
    "bug" \
    "$(cat <<'EOF'
## Summary

The `READINFODEBUG` vendor-request handler in `USBHandler.c` has two bugs
in the same code path:

1. **Last-character clobber:** `glEp0Buffer[len-1] = 0` overwrites the
   last printable character.  `MyDebugSNPrint()` returns `len` excluding
   the NUL, so the NUL should be placed at `glEp0Buffer[len]`, not
   `glEp0Buffer[len-1]`.

2. **EP0 buffer overflow:** `glBufDebug` is 100 bytes (`MAXLEN_D_USB`)
   but `glEp0Buffer` is only 64 bytes (`CYFX_SDRAPP_MAX_EP0LEN`).  The
   `memcpy` is uncapped — if debug text accumulates past 64 bytes between
   host polls, it writes past the end of the DMA allocation.

## Fix

In the `READINFODEBUG` case (`USBHandler.c`):

1. Cap `len` to `CYFX_SDRAPP_MAX_EP0LEN - 1` (63) before the `memcpy`.
2. Change `glEp0Buffer[len-1] = 0` → `glEp0Buffer[len] = '\0'`.
3. Send `len + 1` bytes via `CyU3PUsbSendEP0Data`.

Preserves the documented protocol contract ("last byte is NUL").
Truncation beyond 63 bytes is acceptable for a best-effort debug channel.

## Files

- `SDDC_FX3/USBHandler.c` (~5 lines)

## Validation

- `fx3_cmd` or `rx888_stream`: read `READINFODEBUG` after enabling debug
  mode; verify last character is no longer truncated.
- `tests/fw_test.sh` TAP suite passes.
EOF
)"

# ── Issue #2: Fix #8 — Si5351 ignores I2C errors ────────────────────────

create_issue \
    "Si5351 clock programming ignores I2C write errors" \
    "bug" \
    "$(cat <<'EOF'
## Summary

`SetupPLL()` and `SetupMultisynth()` in `Si5351.c` call `I2cTransfer()`
without checking the return value.  `si5351aSetFrequencyA()` and
`si5351aSetFrequencyB()` also discard the status of their trailing
`I2cTransferW1()` calls (PLL reset, clock-control register).

If the I2C bus wedges during ADC clock programming, the firmware silently
continues with a bad or missing clock — the exact precondition for the
streaming-wedge scenario described in `docs/wedge_detection.md`.

## Fix

1. Change `SetupPLL` and `SetupMultisynth` return type from `void` to
   `CyU3PReturnStatus_t`.  Return their `I2cTransfer` result.
2. In `si5351aSetFrequencyA` / `si5351aSetFrequencyB`, check every return
   value.  On failure: `DebugPrint` the error and return early.
3. Change the return type of both `si5351aSetFrequency{A,B}` from `void`
   to `CyU3PReturnStatus_t`.  Update `Si5351.h`.
4. Update call sites:
   - `STARTADC` in `USBHandler.c`: propagate error → `isHandled = CyFalse`
     (sends STALL to host).
   - `RunApplication.c` (`si5351aSetFrequencyB` during detection): add
     status check + `DebugPrint`.

## Files

- `SDDC_FX3/driver/Si5351.c` (~20 lines)
- `SDDC_FX3/driver/Si5351.h`
- `SDDC_FX3/USBHandler.c` (~3 lines)
- `SDDC_FX3/RunApplication.c` (~2 lines)

## Validation

- Code inspection: every `I2cTransfer` / `I2cTransferW1` call in
  `Si5351.c` now has its return value checked.
- Runtime (if hardware available): corrupt I2C bus or disconnect Si5351;
  verify firmware logs the error and `STARTADC` STALLs EP0.
- `tests/fw_test.sh` TAP suite passes.
EOF
)"

# ── Issue #3: Fix #9 — AD8340 → AD8370 naming ───────────────────────────

create_issue \
    "Rename AD8340_VGA to AD8370_VGA (naming inconsistency)" \
    "cleanup" \
    "$(cat <<'EOF'
## Summary

The RX888mk2 hardware uses the **AD8370** variable-gain amplifier.
`docs/architecture.md` and `rx888r2.c` correctly say AD8370, but the
source-level symbol is `AD8340_VGA` — a typo carried forward from the
original ExtIO_sddc codebase.

## Fix

Rename `AD8340_VGA` → `AD8370_VGA` in:

- `SDDC_FX3/protocol.h` — enum value
- `SDDC_FX3/USBHandler.c` — `SETARGFX3` switch case
- `SDDC_FX3/DebugConsole.c` — `SETARGFX3List[]` string literal
- `docs/LICENSE_ANALYSIS.md` — one prose reference

Wire value stays `11`; no protocol-level change.  Host compatibility is
unaffected (`rx888_tools` uses the numeric value).

## Files

- `SDDC_FX3/protocol.h`
- `SDDC_FX3/USBHandler.c`
- `SDDC_FX3/DebugConsole.c`
- `docs/LICENSE_ANALYSIS.md`

## Validation

- `grep -r AD8340 SDDC_FX3/ docs/` returns zero hits.
- `tests/fw_test.sh` TAP suite passes.
EOF
)"

echo ""
echo "Done.  All issues created in $REPO."
