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

# ═══════════════════════════════════════════════════════════════════
# BUG: fw_test.sh stale-command tests don't distinguish STALL from accepted
# ═══════════════════════════════════════════════════════════════════

create_issue \
  "Bug: fw_test.sh stale-command tests pass even when firmware accepts the command" \
  "bug" \
  "$(cat <<'EOF'
**File:** `tests/fw_test.sh:296-317`
**Severity:** Medium (silent false-positive in regression test)

### Problem

The stale tuner command tests (0xB4, 0xB5, 0xB8) use `run_cmd raw`
and check whether the output starts with `PASS`:

```bash
output=$(run_cmd raw 0xB4) && {
    tap_ok "stale TUNERINIT (0xB4): got STALL as expected"
}
```

But `run_cmd` returns success whenever `[[ "$output" == PASS* ]]`, and
`do_raw()` in `fx3_cmd.c` prints **both**:
- `PASS raw 0xB4: STALL (as expected for removed command)` — correct
- `PASS raw 0xB4: accepted` — command was NOT removed

Both start with `PASS`, so the test passes either way. The test name
says "got STALL as expected" but never actually verifies the word
"STALL" appeared. This is a false-positive regression test for exactly
the behavior it claims to check — confirming that removed commands are
actually rejected.

This is likely why an earlier test run did not flag a command that was
still being accepted rather than stalled.

### Fix

Check for `STALL` in the output instead of relying on `run_cmd`
success:

```bash
output=$("$FX3_CMD" raw 0xB4 2>&1) || true
if [[ "$output" == *STALL* ]]; then
    tap_ok "stale TUNERINIT (0xB4): STALL as expected"
else
    tap_fail "stale TUNERINIT (0xB4): command accepted (not removed?)" "$output"
fi
```

Apply the same pattern to the 0xB5 and 0xB8 checks.
EOF
)"

# ═══════════════════════════════════════════════════════════════════
# BUG: fx3_cmd do_test() doesn't validate TESTFX3 reply length
# ═══════════════════════════════════════════════════════════════════

create_issue \
  "Bug: fx3_cmd do_test() reports PASS on short TESTFX3 reply" \
  "bug" \
  "$(cat <<'EOF'
**File:** `tests/fx3_cmd.c:163-178`
**Severity:** Low (masks broken firmware during bring-up)

### Problem

`do_test()` checks `r < 0` but not `r < 4`:

```c
uint8_t buf[4] = {0};
int r = ctrl_read(h, TESTFX3, 0, 0, buf, 4);
if (r < 0) {
    printf("FAIL test: %s\n", libusb_strerror(r));
    return 1;
}
uint8_t hwconfig   = buf[0];
uint8_t fw_major   = buf[1];
uint8_t fw_minor   = buf[2];
uint8_t rqt_cnt    = buf[3];
printf("PASS test: hwconfig=0x%02X fw=%d.%d vendorRqtCnt=%d\n",
       hwconfig, fw_major, fw_minor, rqt_cnt);
```

`libusb_control_transfer` returns the number of bytes actually
transferred, which could be 0-3 on a malformed firmware response.
Since `buf` is zero-initialized, a short read silently produces:

```
PASS test: hwconfig=0x04 fw=2.0 vendorRqtCnt=0
```

This looks valid but is wrong — the firmware only sent 2 bytes. During
automated bring-up with a new firmware build, this hides the problem.

### Fix

Require exactly 4 bytes:

```c
if (r < 4) {
    printf("FAIL test: short reply (%d bytes, expected 4)\n", r);
    return 1;
}
```
EOF
)"

echo ""
echo "=== Creating enhancement issues ==="
echo ""

# ═══════════════════════════════════════════════════════════════════
# ENHANCEMENT: Add debug-over-USB console to fx3_cmd
# ═══════════════════════════════════════════════════════════════════

create_issue \
  "Enhancement: add debug-over-USB console subcommand to fx3_cmd" \
  "enhancement" \
  "$(cat <<'EOF'
**Files:** `tests/fx3_cmd.c`, `SDDC_FX3/USBhandler.c:270-316`
**Reference:** `docs/debugging.md`

### Background

The FX3 firmware has a built-in debug-over-USB path (enabled when
`_DEBUG_USB_` is defined, which is the current default). It supports
bidirectional communication over EP0 control transfers:

- **Output:** firmware buffers `DebugPrint2USB` messages in
  `bufdebug[100]`; host retrieves them by polling `READINFODEBUG`
  (0xBA) with `wValue=0`.
- **Input:** host sends one character per `READINFODEBUG` call via
  `wValue=char`; firmware accumulates them in `ConsoleInBuffer` and
  executes on CR (0x0D), giving access to the same `ParseCommand`
  console (threads, stack, gpif, reset).
- **Activation:** host must first send `TESTFX3` (0xAC) with
  `wValue=1` to set `flagdebug=true`; without this, `DebugPrint2USB`
  returns immediately and nothing is buffered.

The host tool `fx3_cmd.c` already has all the USB control transfer
infrastructure (`ctrl_read`, `ctrl_write_u32`, etc.) and every other
vendor command, but has **no support for READINFODEBUG** and its
`do_test()` always sends `wValue=0` (which disables debug mode).

### What's needed

**1. Add missing protocol constants**

```c
#define READINFODEBUG 0xBA
#define MAXLEN_D_USB  100
```

**2. Add a `do_debug()` interactive console**

The firmware packs both directions into a single EP0 transaction, so
every poll can piggyback a character:

```
Host sends:  vendor IN, bRequest=0xBA, wValue=char_or_0, wLength=100
Firmware:    (1) if wValue > 0, push char into ConsoleInBuffer
             (2) if debug output buffered, return it in data phase
             (3) else STALL EP0
```

The host-side loop would:

1. Send `TESTFX3` with `wValue=1` to enable debug mode.
2. Loop:
   - Check stdin (non-blocking, e.g. `select` or `poll`) for a typed
     character.
   - Call `ctrl_read(h, READINFODEBUG, ch, 0, buf, MAXLEN_D_USB)`.
   - If `r > 0`: print the returned debug string to stdout.
   - If `r == LIBUSB_ERROR_PIPE` (STALL): no data — sleep ~50 ms and
     poll again.
3. On Ctrl-C, send `TESTFX3` with `wValue=0` to disable debug mode.

**3. Wire it into main()**

Add `debug` to the usage string and command dispatch, following the
existing pattern.

### Protocol notes

- `READINFODEBUG` is a **vendor IN** request (device sends data to
  host via `CyU3PUsbSendEP0Data`).
- STALL on EP0 means "no debug output available" — not an error. The
  host should treat `LIBUSB_ERROR_PIPE` as "try again later."
- The firmware null-terminates at `len-1`, truncating the last byte of
  each response (known issue documented in `docs/debugging.md`).
- The firmware forces all input to lowercase (`| 0x20`), so
  case-sensitive commands are not possible.
- EP0 control path is independent of bulk streaming, so `fx3_cmd debug`
  can run while `rx888_stream` is active (from a separate process with
  its own libusb handle).

### Usage

```
fx3_cmd debug         # interactive console to FX3 firmware
```

Then type `?`, `threads`, `stack`, `gpif`, `reset` as you would on
the UART serial console.
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
