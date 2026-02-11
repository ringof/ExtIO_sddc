#!/usr/bin/env bash
set -euo pipefail

# Create GitHub issues for bugs found during post-cleanup firmware review.
# Run from anywhere with gh authenticated:
#   ./scripts/create_issues.sh
#
# Prerequisites:
#   - gh CLI installed and authenticated (gh auth status)
#   - Write access to the target repo

REPO="ringof/ExtIO_sddc"
DELAY=2  # seconds between API calls to avoid rate limiting

created=0
failed=0

create_issue() {
    local title="$1"
    local labels="$2"
    local body="$3"

    echo "Creating: $title"
    if gh issue create --repo "$REPO" --title "$title" --label "$labels" --body "$body"; then
        created=$((created + 1))
    else
        echo "  FAILED: $title" >&2
        failed=$((failed + 1))
    fi
    sleep "$DELAY"
}

# Ensure labels exist
echo "=== Ensuring labels exist ==="
gh label create "bug"           --repo "$REPO" --color "d73a4a" --description "Something isn't working"                  2>/dev/null || true
gh label create "enhancement"   --repo "$REPO" --color "a2eeef" --description "New feature or request"                   2>/dev/null || true
gh label create "cleanup"       --repo "$REPO" --color "0e8a16" --description "Code cleanup and quality"                 2>/dev/null || true

echo ""
echo "=== Creating bug fix issues ==="
echo ""

# ═══════════════════════════════════════════════════════════════════
# BUGS: TraceSerial out-of-bounds array reads
# ═══════════════════════════════════════════════════════════════════

create_issue \
  "Bug: TraceSerial indexes SETARGFX3List out of bounds on untrusted wIndex" \
  "bug" \
  "$(cat <<'EOF'
**File:** `SDDC_FX3/USBhandler.c:69` (via `SDDC_FX3/DebugConsole.c:58-61`)
**Severity:** Medium

### Problem

`TraceSerial` indexes the `SETARGFX3List[]` array directly with the
host-supplied `wIndex` value (a `uint16_t` from the USB SETUP packet)
with no bounds check:

```c
case SETARGFX3:
    DebugPrint(4, "%s\t%d", SETARGFX3List[wIndex], wValue);
    break;
```

`SETARGFX3List` has only 14 entries (indices 0-13). Any `wIndex >= 14`
causes an **out-of-bounds read**, which on the FX3 ARM9 core could
return garbage or cause a data abort.

After the variant cleanup, only `DAT31_ATT` (10) and `AD8340_VGA` (11)
are valid; the entries for removed R82xx arguments (1-4) and removed
radio variants (`PRESELECTOR` at 12, `VHF_ATTENUATOR` at 13) are
stale placeholders.

### Fix
Add a bounds check before indexing:
```c
case SETARGFX3:
    if (wIndex < sizeof(SETARGFX3List)/sizeof(SETARGFX3List[0]))
        DebugPrint(4, "%s\t%d", SETARGFX3List[wIndex], wValue);
    else
        DebugPrint(4, "ARG(%d)\t%d", wIndex, wValue);
    break;
```

Also clean up the stale entries: replace `"PRESELECTOR"` and
`"VHF_ATTENUATOR"` with numeric placeholders `"12"` and `"13"`.
EOF
)"

create_issue \
  "Bug: TraceSerial indexes FX3CommandName out of bounds on unknown vendor requests" \
  "bug" \
  "$(cat <<'EOF'
**File:** `SDDC_FX3/USBhandler.c:65` (via `SDDC_FX3/DebugConsole.c:52-55`)
**Severity:** Medium

### Problem

`TraceSerial` computes the array index as `bRequest - 0xAA` with no
range check:

```c
DebugPrint(4, "%s\t", FX3CommandName[bRequest - 0xAA]);
```

`FX3CommandName` has 17 entries (indices 0-16, covering commands
0xAA-0xBA). `TraceSerial` is called for **every** vendor request after
the main switch, including the `default:` case.

If a host sends a vendor request with `bRequest > 0xBA` (e.g. 0xBB),
the index is 17 — past the end of the array. For `bRequest < 0xAA`,
the subtraction produces a negative value, causing undefined behavior
when used as an array index.

### Fix
Guard with a range check:
```c
if (bRequest >= 0xAA && bRequest <= 0xBA)
    DebugPrint(4, "%s\t", FX3CommandName[bRequest - 0xAA]);
else
    DebugPrint(4, "CMD(0x%x)\t", bRequest);
```
EOF
)"

# ═══════════════════════════════════════════════════════════════════
# BUG: SETARGFX3 protocol oddity for removed arguments
# ═══════════════════════════════════════════════════════════════════

create_issue \
  "Bug: SETARGFX3 consumes EP0 data phase before validating wIndex" \
  "bug" \
  "$(cat <<'EOF'
**File:** `SDDC_FX3/USBhandler.c:220-237`
**Severity:** Low

### Problem

The `SETARGFX3` handler calls `CyU3PUsbGetEP0Data()` unconditionally
before checking whether `wIndex` is a recognized argument:

```c
case SETARGFX3:
{
    int rc = -1;
    CyU3PUsbGetEP0Data(wLength, glEp0Buffer, NULL);  // ACKs data phase
    switch(wIndex) {
        case DAT31_ATT:  ... rc = 0; break;
        case AD8340_VGA: ... rc = 0; break;
    }
    vendorRqtCnt++;
    if (rc == 0)
        isHandled = CyTrue;
}
```

For unrecognized `wIndex` values (including the removed R82xx arguments
1-4), the data phase is already ACKed by `GetEP0Data`, then
`isHandled` stays `CyFalse`, which stalls the status phase. This
creates a DATA-acked-but-STATUS-stalled protocol violation.

Contrast with the removed tuner commands (0xB4, 0xB5, 0xB8) which hit
the outer `default:` and stall cleanly from the start.

Additionally, `vendorRqtCnt` is incremented even for rejected arguments.

### Suggestion
Either validate `wIndex` before calling `GetEP0Data`, or set
`isHandled = CyTrue` and stall explicitly for unknown sub-arguments.
EOF
)"

# ═══════════════════════════════════════════════════════════════════
# BUG: LED_YELLOW and LED_BLUE not driven
# ═══════════════════════════════════════════════════════════════════

create_issue \
  "Bug: LED_YELLOW and LED_BLUE not driven by rx888r2_GpioSet" \
  "bug,low-priority" \
  "$(cat <<'EOF'
**Files:** `SDDC_FX3/radio/rx888r2.c:17-27, 29-53`
**Severity:** Low (cosmetic — only LEDs affected)

### Problem

`rx888r2_GpioInitialize()` calls:
```c
rx888r2_GpioSet(LED_RED | LED_YELLOW | LED_BLUE);
```

But `rx888r2_GpioSet()` only drives 8 GPIOs:
`SHDWN`, `DITH`, `RANDO`, `BIAS_HF`, `BIAS_VHF`, `LED_RED`, `PGA_EN`,
`VHF_EN`.

There are no `GPIO_LED_YELLOW` or `GPIO_LED_BLUE` pin defines, no
`ConfGPIOsimpleout()` calls for them, and no `CyU3PGpioSetValue()`
calls. The `LED_YELLOW` (bit 10) and `LED_BLUE` (bit 12) bits in the
control word are silently ignored.

After the multi-variant cleanup, there is no other code path that
could drive these LEDs. The host can send GPIOFX3 with these bits set,
but they have no effect.

### Questions
- Does the RX888mk2 hardware have yellow and blue LEDs? If so, which
  FX3 GPIO pins are they connected to?
- If the LEDs don't exist on this variant, the enum values should be
  removed to avoid confusion.
EOF
)"

echo ""
echo "=== Creating cleanup issues ==="
echo ""

# ═══════════════════════════════════════════════════════════════════
# CLEANUP: Dead code left behind by variant removal
# ═══════════════════════════════════════════════════════════════════

create_issue \
  "Cleanup: dead code left behind by variant removal" \
  "cleanup" \
  "$(cat <<'EOF'
**Severity:** Cosmetic (no functional impact)

### Problem

Several pieces of dead code were left behind during the multi-variant
and R82xx cleanup:

**1. Dangling `WriteAttenuator` extern**
`SDDC_FX3/USBhandler.c:24`:
```c
extern void WriteAttenuator(uint8_t value);
```
This function no longer exists. It was part of the removed multi-variant
abstraction. Not a link error (never called), but a landmine if someone
calls it.

**2. Dead `si5351aSetFrequencyB` declaration in USBhandler.c**
`SDDC_FX3/USBhandler.c:27`:
```c
void si5351aSetFrequencyB(UINT32 freq2);
```
Only called from `RunApplication.c` (for tuner reference clock during
HW detection). Never called from `USBhandler.c`. Leftover from when
`TUNERSTDBY` called `si5351aSetFrequencyB(0)`.

**3. Duplicate rx888r2 function prototypes**
The same four `rx888r2_*` function declarations appear in both:
- `SDDC_FX3/Application.h:94-98`
- `SDDC_FX3/radio/radio.h:4-7`

This was introduced during simplification from multi-variant to
single-variant. It's a maintenance hazard — change one, forget the
other.

### Fix
1. Delete the `WriteAttenuator` extern from `USBhandler.c`.
2. Delete the `si5351aSetFrequencyB` declaration from `USBhandler.c`.
3. Remove the rx888r2 prototypes from `Application.h` (keep them in
   `radio.h` which is their natural home), and add `#include "radio.h"`
   where needed.
EOF
)"

echo ""
echo "=== Done ==="
echo "Created: $created issues"
echo "Failed:  $failed issues"
