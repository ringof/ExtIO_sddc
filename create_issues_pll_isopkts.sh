#!/usr/bin/env bash
# create_issues_pll_isopkts.sh — Create GitHub issues for PLAN-getstats-pll-isopkts.md
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

# ── Issue 1: Add si5351_status (PLL lock) to GETSTATS response ───────────

create_issue \
    "GETSTATS: add si5351_status byte for PLL lock detection" \
    "enhancement" \
    "$(cat <<'EOF'
## Summary

Extend the GETSTATS (`0xB3`) response from 19 → 20 bytes by appending
the Si5351A device-status register (register 0) as a new field at
offset 19.  This was explicitly deferred during the initial GETSTATS
implementation as "planned as next extension."

The Si5351 register 0 bit layout:
- Bit 5: PLL A loss-of-lock (1 = unlocked)
- Bit 6: PLL B loss-of-lock (1 = unlocked)
- Bit 7: SYS_INIT (1 = device still initializing)

PLL A drives the ADC clock — if unlocked, all sample data is garbage.
This is the most direct indicator of clock health available.

## Changes

**Firmware (`SDDC_FX3/USBHandler.c`, case GETSTATS):**

After the EP underrun `memcpy`, add:

```c
uint8_t si_status = 0;
I2cTransfer(0x00, SI5351_ADDR, 1, &si_status, CyTrue);
glEp0Buffer[off++] = si_status;                          // [19]
```

**Host test tool (`tests/fx3_cmd.c`):**

- Update `GETSTATS_LEN` 19 → 20
- Add `uint8_t si5351_status` to `struct fx3_stats`
- Parse `buf[19]` in `read_stats()`
- Print `pll=0x%02X` in `do_stats()`
- Add `do_test_stats_pll()` subcommand: verify PLL A locked and
  SYS_INIT clear

**Docs:**

- `docs/diagnostics_side_channel.md`: move `si5351_status` from
  "Not yet implemented" to implemented table; update byte counts
- `SDDC_FX3/docs/debugging.md` §5: add offset 19 row
- `docs/architecture.md`: update "19 B IN" → "20 B IN"

## Cost

- One I2C register read (~50 µs) per GETSTATS poll
- No I2C contention during streaming (attenuator/VGA use GPIO bit-bang)
- If I2C read fails, `si_status` stays 0x00 (= "PLLs locked, init done")
  and `glCounter[1]` increments — safe default

## Backward compatibility

Old host code requesting `wLength=19` receives 19 bytes unchanged;
the 20th byte is simply not transferred.

## Validation

- `fx3_cmd stats` prints PLL status byte
- `fx3_cmd stats_pll` verifies PLL A locked and SYS_INIT clear
- `fw_test.sh` TAP suite passes

## Ref

- Plan: `PLAN-getstats-pll-isopkts.md`, Task 1
EOF
)"

# ── Issue 2: Fix isoPkts = 1 on bulk endpoint ────────────────────────────

create_issue \
    "Fix epCfg.isoPkts = 1 on bulk endpoint (should be 0)" \
    "bug" \
    "$(cat <<'EOF'
## Summary

`StartStopApplication.c:99` sets `epCfg.isoPkts = 1` on EP1 IN, which
is configured as a **bulk** endpoint (`CY_U3P_USB_EP_BULK`).

Per the FX3 SDK (`cyu3usb.h:446`), `isoPkts` is "Number of packets per
micro-frame for **ISO** endpoints."  Non-zero values are only valid on
endpoints 3 and 7 (`cyu3usb.h:929`).  On a bulk EP1, the SDK ignores
the field — but the non-zero value is misleading and could mask a
copy-paste bug or cause an error if the endpoint type were ever changed.

## Fix

```diff
- epCfg.isoPkts = 1;
+ epCfg.isoPkts = 0;   /* bulk endpoint — isoPkts is for ISO EPs only */
```

This is a no-op at runtime.  Setting to 0 matches the `CyU3PMemSet`
zero-fill two lines earlier and makes the intent explicit.

## Files

- `SDDC_FX3/StartStopApplication.c` (1 line)

## Validation

- Code inspection: field now matches SDK expectations for bulk endpoints
- `fw_test.sh` TAP suite passes (no runtime behavior change)

## Ref

- Plan: `PLAN-getstats-pll-isopkts.md`, Task 2
EOF
)"

# ── Issue 3: Add stats_pll test to fw_test.sh ────────────────────────────

create_issue \
    "Add stats_pll test case to fw_test.sh" \
    "enhancement" \
    "$(cat <<'EOF'
## Summary

After implementing the `si5351_status` field in GETSTATS (see companion
issue "GETSTATS: add si5351_status byte for PLL lock detection"), add a
new TAP test case to `tests/fw_test.sh` that exercises it.

## Changes

**`tests/fw_test.sh`:**

Add a new test after the existing stats tests (tests 22–25) that runs
`fx3_cmd stats_pll` and verifies:

- Command exits 0 (PASS)
- Output contains "PASS" (PLL A locked, SYS_INIT clear)

## Dependencies

- Requires the `stats_pll` subcommand in `fx3_cmd` (part of the
  GETSTATS si5351_status issue)

## Validation

- `fw_test.sh` with device connected: new test case passes
- Without device: test is skipped (existing skip logic)

## Ref

- Plan: `PLAN-getstats-pll-isopkts.md`, Task 3
EOF
)"

# ── Issue 4: Update GETSTATS layout comment in fx3_cmd.c ─────────────────

create_issue \
    "Update GETSTATS layout comment in fx3_cmd.c for 20-byte response" \
    "cleanup" \
    "$(cat <<'EOF'
## Summary

The GETSTATS response layout comment block in `tests/fx3_cmd.c`
(lines 795–802) documents the 19-byte wire format.  After adding the
`si5351_status` field at offset 19, update this comment to include the
new field and reflect the 20-byte total.

## Changes

**`tests/fx3_cmd.c` (comment block):**

Add:
```
 *   [19]     uint8   Si5351 device status (PLL lock bits)
```

Update the header line to say "20 bytes" instead of "19 bytes".

## Files

- `tests/fx3_cmd.c` (comment only, ~2 lines)

## Validation

- Code inspection: comment matches actual wire format

## Ref

- Plan: `PLAN-getstats-pll-isopkts.md`, Task 4
EOF
)"

echo ""
echo "Done.  All issues created in $REPO."
