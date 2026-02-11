#!/usr/bin/env bash
set -euo pipefail

# Create GitHub issues for SDDC_FX3 firmware plan items and code review bugs.
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
gh label create "license"       --repo "$REPO" --color "fbca04" --description "Licensing concern"                        2>/dev/null || true
gh label create "testing"       --repo "$REPO" --color "c5def5" --description "Test infrastructure"                      2>/dev/null || true
gh label create "documentation" --repo "$REPO" --color "0075ca" --description "Documentation improvements"               2>/dev/null || true
gh label create "low-priority"  --repo "$REPO" --color "ededed" --description "Nice to have, not urgent"                 2>/dev/null || true

echo ""
echo "=== Creating plan items ==="
echo ""

# ═══════════════════════════════════════════════════════════════════
# PLAN ITEMS
# ═══════════════════════════════════════════════════════════════════

create_issue \
  "Remove GPL-licensed R82xx tuner driver" \
  "license,cleanup" \
  "$(cat <<'EOF'
## Phase 2, Step 1

**Problem:** The R82xx tuner driver (`tuner_r82xx.c`, `tuner_r82xx.h`) is
licensed under GPL v2+, which is incompatible with the proprietary Cypress
SDK linked into the same binary. Removing the tuner code eliminates this
conflict. See `docs/LICENSE_ANALYSIS.md` for the full analysis.

**Key finding:** The R82xx driver is NOT needed for hardware detection. The
detection code in `RunApplication.c` only does a raw I2C probe at address
`0x74` — it never calls any R82xx driver function.

### Files to delete
- `SDDC_FX3/driver/tuner_r82xx.c`
- `SDDC_FX3/driver/tuner_r82xx.h`

### Files to edit

**SDDC_FX3/makefile:**
- Remove `tuner_r82xx.c` from `DRIVERSRC`

**SDDC_FX3/RunApplication.c:**
- Remove `#include "tuner_r82xx.h"`
- Add local define: `#define R828D_I2C_ADDR 0x74` (for HW detection only)

**SDDC_FX3/USBhandler.c:**
- Remove `#include "tuner_r82xx.h"`
- Remove global tuner state (`struct r82xx_priv tuner`, `struct r82xx_config
  tuner_config`, extern declarations for gain functions)
- Remove `r820_initialize()` function
- Remove `TUNERINIT` (0xB4), `TUNERTUNE` (0xB5), `TUNERSTDBY` (0xB8) handlers
- In `SETARGFX3` (0xB6): remove cases for `R82XX_ATTENUATOR` (1),
  `R82XX_VGA` (2), `R82XX_SIDEBAND` (3). Keep `DAT31_ATT` (10) and
  `AD8340_VGA` (11)

**SDDC_FX3/protocol.h:**
- Remove `TUNERINIT`, `TUNERTUNE`, `TUNERSTDBY` from `FX3Command` enum
- Remove `R82XX_ATTENUATOR`, `R82XX_VGA`, `R82XX_SIDEBAND`, `R82XX_HARMONIC`
  from `ArgumentList` enum

### Compatibility
- An older host sending tuner commands will receive USB STALL, which is safe
- `rx888_stream` sends `TUNERSTDBY` as best-effort; HF streaming is unaffected

### Verification
- `make clean && make all` must succeed
- `tests/fw_test.sh` must pass (15/15)

### Reference
`PLAN.md`, Phase 2 Step 1
EOF
)"

create_issue \
  "Remove dead UINT64 macro from Application.h" \
  "cleanup" \
  "$(cat <<'EOF'
## Cleanup: dead typedef

**File:** `SDDC_FX3/Application.h:59`

```c
#define UINT64  uint32_t
```

This defines `UINT64` as a 32-bit type. After the R82xx removal, the only
reference to `UINT64` is a comment in `protocol.h`. The macro is dead code
but a dangerous landmine if anyone uses it expecting 64-bit storage.

### Fix
Delete the `#define UINT64 uint32_t` line. Remove the comment reference
in `protocol.h` (change `UINT64` to `uint64_t` in the comment).

### Reference
`SDDC_FX3_REVIEW_ISSUES.md`, Issue 3
EOF
)"

echo ""
echo "=== Creating bug fix issues (high severity) ==="
echo ""

# ═══════════════════════════════════════════════════════════════════
# HIGH SEVERITY BUGS
# ═══════════════════════════════════════════════════════════════════

create_issue \
  "Bug: sizeof on pointer instead of struct in StopApplication()" \
  "bug" \
  "$(cat <<'EOF'
**File:** `SDDC_FX3/StartStopApplication.c:157`
**Severity:** High

### Problem

```c
CyU3PMemSet((uint8_t *)&epConfig, 0, sizeof(&epConfig));
```

`sizeof(&epConfig)` evaluates to `sizeof(CyU3PEpConfig_t*)` which is 4 bytes
on ARM, **not** the size of the `CyU3PEpConfig_t` struct. Only 4 bytes of
the endpoint config are zeroed before passing to `CyU3PSetEpConfig()`.

### Impact
The endpoint may not be properly disabled on USB disconnect/reset events.
Remaining uninitialized fields could leave stale DMA state, causing issues
on reconnection.

### Fix
```c
CyU3PMemSet((uint8_t *)&epConfig, 0, sizeof(epConfig));
```

### Reference
`SDDC_FX3_REVIEW_ISSUES.md`, Issue 1
EOF
)"

create_issue \
  "Bug: sizeof on pointer in I2C read handler zeroes only 4 bytes" \
  "bug" \
  "$(cat <<'EOF'
**File:** `SDDC_FX3/USBhandler.c:244`
**Severity:** High

### Problem

```c
CyU3PMemSet (glEp0Buffer, 0, sizeof (glEp0Buffer));
```

`glEp0Buffer` is declared as `uint8_t *glEp0Buffer`, so `sizeof(glEp0Buffer)`
is 4 (pointer size on ARM), not the allocated buffer size of 64 bytes
(`CYFX_SDRAPP_MAX_EP0LEN`).

### Impact
In the `I2CRFX3` handler, only 4 bytes of the EP0 buffer are zeroed before
the I2C read. If the I2C read returns fewer bytes than `wLength`, stale data
from previous vendor requests may be sent to the host.

### Fix
```c
CyU3PMemSet (glEp0Buffer, 0, CYFX_SDRAPP_MAX_EP0LEN);
```

### Reference
`SDDC_FX3_REVIEW_ISSUES.md`, Issue 2
EOF
)"

echo ""
echo "=== Creating bug fix issues (medium severity) ==="
echo ""

# ═══════════════════════════════════════════════════════════════════
# MEDIUM SEVERITY BUGS
# ═══════════════════════════════════════════════════════════════════

create_issue \
  "Bug: I2C write errors silently reported as success to host" \
  "bug" \
  "$(cat <<'EOF'
**File:** `SDDC_FX3/USBhandler.c:228-239`
**Severity:** Medium

### Problem

```c
apiRetStatus = I2cTransfer(wIndex, wValue, wLength, glEp0Buffer, CyFalse);
if (apiRetStatus == CY_U3P_SUCCESS)
    isHandled = CyTrue;
else
{
    CyU3PDebugPrint (4, "I2cwrite Error %d\n", apiRetStatus);
    isHandled = CyTrue; // ?!?!?!
}
```

The original developer flagged this with `?!?!?!`. When an I2C write fails,
`isHandled` is still set to `CyTrue`, causing the USB stack to ACK the
control transfer. The host has no way to detect the failure.

### Impact
Hardware configuration failures (clock generator, attenuator) are invisible
to host software.

### Fix
Set `isHandled = CyFalse` on I2C failure so the endpoint STALLs, signaling
the error to the host.

### Reference
`SDDC_FX3_REVIEW_ISSUES.md`, Issue 4
EOF
)"

create_issue \
  "Bug: No wLength validation on EP0 vendor requests" \
  "bug" \
  "$(cat <<'EOF'
**File:** `SDDC_FX3/USBhandler.c` (10 call sites)
**Severity:** Medium

### Problem

The vendor request handler calls `CyU3PUsbGetEP0Data(wLength, glEp0Buffer, NULL)`
throughout without validating that `wLength <= CYFX_SDRAPP_MAX_EP0LEN` (64 bytes).
The `wLength` value comes directly from the USB setup packet and is controlled
by the host.

If the host sends a vendor request with `wLength > 64`, this could overflow
the 64-byte `glEp0Buffer`.

Affected commands: `GPIOFX3`, `STARTADC`, `I2CWFX3`, `TUNERINIT`, `TUNERSTDBY`,
`TUNERTUNE`, `SETARGFX3`, `STARTFX3`, `STOPFX3`, `RESETFX3`.

### Fix
Add a check at the top of the vendor request handler:
```c
if (wLength > CYFX_SDRAPP_MAX_EP0LEN) {
    CyU3PUsbStall(0, CyTrue, CyFalse);
    return CyTrue;
}
```

### Reference
`SDDC_FX3_REVIEW_ISSUES.md`, Issue 5
EOF
)"

create_issue \
  "Bug: CheckStatus hangs forever on error with no recovery" \
  "bug" \
  "$(cat <<'EOF'
**Files:** `SDDC_FX3/Support.c:139-143`, `SDDC_FX3/StartUP.c:17-38`
**Severity:** Medium

### Problem

```c
void CheckStatus(char* StringPtr, CyU3PReturnStatus_t Status)
{
    if (Status != CY_U3P_SUCCESS) {
        DebugPrint(4, "\r\n%s failed...", ...);
        while (1) { DebugPrint(4, "?"); }  // infinite loop
    }
}
```

Additionally, `IndicateError()` in `StartUP.c` is a no-op (`return;`
immediately), so there is no LED or GPIO indication of failure.

### Impact
In a deployed device without UART, a transient initialization failure
(e.g., I2C bus glitch) causes the device to hang permanently with no
user-visible indication. The only recovery is physical power cycling.

### Suggestion
Log the error and call `CyU3PDeviceReset(CyFalse)` after a timeout,
or implement `IndicateError()` to blink an LED pattern.

### Reference
`SDDC_FX3_REVIEW_ISSUES.md`, Issue 8
EOF
)"

echo ""
echo "=== Creating design/quality issues ==="
echo ""

# ═══════════════════════════════════════════════════════════════════
# DESIGN & QUALITY ISSUES
# ═══════════════════════════════════════════════════════════════════

create_issue \
  "Race condition on debug-over-USB buffer" \
  "bug,low-priority" \
  "$(cat <<'EOF'
**Files:** `SDDC_FX3/DebugConsole.c:245-250`, `SDDC_FX3/USBhandler.c:356-388`
**Severity:** Medium (only affects debug output, not production streaming)

### Problem

The debug-over-USB mechanism uses a shared buffer (`bufdebug[]`) and length
counter (`debtxtlen`, declared `volatile`). Writer (DebugPrint2USB) and reader
(READINFODEBUG EP0 handler) access the shared buffer from different execution
contexts without synchronization.

The `memcpy` and `debtxtlen` update are not atomic, so concurrent access can
corrupt the buffer or cause partial reads.

### Suggestion
Use a mutex, or disable/restore interrupts around the critical sections,
or use a proper ring buffer with atomic indices.

### Reference
`SDDC_FX3_REVIEW_ISSUES.md`, Issue 6
EOF
)"

create_issue \
  "Soft-float arithmetic in Si5351 PLL calculations on FPU-less ARM9" \
  "enhancement" \
  "$(cat <<'EOF'
**File:** `SDDC_FX3/driver/Si5351.c:91-94, 178-181`
**Severity:** Medium

### Problem

The Si5351 PLL setup uses `float` and `double` arithmetic:

```c
P1 = (UINT32)(128 * ((float)num / (float)denom));
f = (double)l;
f *= 1048575;
f /= xtalFreq;
```

The FX3's ARM926EJ-S core has no hardware FPU. All floating-point operations
are emulated in software. This is slow and adds code size for the soft-float
library.

### Fix
The Si5351 PLL register calculations can be performed entirely with integer
arithmetic using the formulas from the Si5351 datasheet:
```c
P1 = 128 * mult + ((128 * num) / denom) - 512;
P2 = 128 * num - denom * ((128 * num) / denom);
P3 = denom;
```

### Reference
`SDDC_FX3_REVIEW_ISSUES.md`, Issue 7
EOF
)"

create_issue \
  "PIB error callback disabled — silent GPIF data errors" \
  "enhancement,low-priority" \
  "$(cat <<'EOF'
**File:** `SDDC_FX3/StartStopApplication.c:130`
**Severity:** Low

### Problem

```c
//  CyU3PPibRegisterCallback(Pib_error_cb, CYU3P_PIB_INTR_ERROR);
```

The GPIF Parallel Interface Block error callback is commented out. Bus
errors such as GPIF data overflows or protocol violations are silently
ignored.

### Impact
At high sample rates, if the USB host cannot consume data fast enough,
GPIF buffers may overflow without any notification. The host would receive
corrupted or discontinuous ADC samples with no indication.

### Suggestion
Re-enable the callback and either log the error or set a flag that the
host can query via `TESTFX3`.

### Reference
`SDDC_FX3_REVIEW_ISSUES.md`, Issue 10
EOF
)"

create_issue \
  "USB 2.0 descriptor omits serial number string index" \
  "bug,low-priority" \
  "$(cat <<'EOF'
**File:** `SDDC_FX3/USBdescriptor.c:52`
**Severity:** Low

### Problem

The USB 3.0 device descriptor specifies serial number string index 3,
but the USB 2.0 descriptor sets it to 0 (none):

```c
// USB 3.0:
0x03,  /* Serial number string index */
// USB 2.0:
0x00,  /* Serial number string index */
```

### Impact
When connected via USB 2.0, the host OS cannot distinguish between
multiple identical SDDC devices by serial number.

### Fix
Change the USB 2.0 descriptor serial number string index from `0x00`
to `0x03`.

### Reference
`SDDC_FX3_REVIEW_ISSUES.md`, Issue 13
EOF
)"

create_issue \
  "Thread stack size (1KB) may be insufficient" \
  "enhancement,low-priority" \
  "$(cat <<'EOF'
**File:** `SDDC_FX3/Application.h:35`
**Severity:** Low

### Problem

```c
#define FIFO_THREAD_STACK (0x400)  // 1024 bytes
```

The single application thread has a 1KB stack. This thread executes I2C
transactions, Si5351 initialization, `DebugPrint2USB` with a 100-byte
local buffer, `ParseCommand`, and `va_list` processing.

The existing "stack" debug command exists specifically to monitor free
stack space, suggesting this was a known concern during development.

### Suggestion
Consider increasing to `0x800` (2KB) and monitoring with the stack debug
command during development.

### Reference
`SDDC_FX3_REVIEW_ISSUES.md`, Issue 15
EOF
)"

create_issue \
  "Console input buffer off-by-one in bounds check" \
  "bug,low-priority" \
  "$(cat <<'EOF'
**Files:** `SDDC_FX3/DebugConsole.c:274-276`, `SDDC_FX3/USBhandler.c:368-370`
**Severity:** Low

### Problem

```c
ConsoleInBuffer[ConsoleInIndex] = InputChar | 0x20;
if (ConsoleInIndex++ < sizeof(ConsoleInBuffer)) ConsoleInBuffer[ConsoleInIndex] = 0;
else ConsoleInIndex--;
```

`ConsoleInBuffer` is 32 bytes. When `ConsoleInIndex` is 31:
- Writes char at [31]
- `31 < 32` is true, increments to 32
- Writes null terminator at [32] — **one byte past the buffer**

The same logic is duplicated in both `UartCallback` and `READINFODEBUG`.

### Fix
Change the check to prevent the out-of-bounds write:
```c
ConsoleInBuffer[ConsoleInIndex] = InputChar | 0x20;
if (ConsoleInIndex < sizeof(ConsoleInBuffer) - 1) {
    ConsoleInIndex++;
    ConsoleInBuffer[ConsoleInIndex] = 0;
}
```

### Reference
`SDDC_FX3_REVIEW_ISSUES.md`, Issue 16
EOF
)"

echo ""
echo "=== Done ==="
echo "Created: $created issues"
echo "Failed:  $failed issues"
