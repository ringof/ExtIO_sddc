# SDDC_FX3 Firmware — Cleanup & Consolidation Plan

## Completed Work

### Phase 1: Variant Removal & Initial Cleanup
- Removed all non-RX888r2 radio variants and their drivers
- Simplified dispatch in RunApplication.c and USBhandler.c to RX888r2-only
- Internalized Interface.h into self-contained protocol.h
- Cleaned up top-level orphaned files
- Removed GPL-licensed R82xx tuner driver and all tuner commands
- Fixed sizeof-on-pointer bugs (StopApplication, I2C read handler)
- Fixed UINT64 typedef (was uint32_t), removed macro
- Fixed I2C write error reporting (was silently ACKing failures)
- Converted Si5351 PLL calculations from soft-float to integer-only
- Replaced CheckStatus/CheckStatusSilent infinite loops with LED blink + reset
- Fixed hardware detection to default to NORADIO instead of RX999
- Fixed USB 2.0 descriptor serial number string index
- Fixed ConsoleInBuffer off-by-one bounds check
- Fixed debug string typos ("Detectedinitialize", "out of rage")
- Re-enabled PIB error callback (issue #10)
- Increased thread stack to 2KB, fixed watermark scan (issue #12)
- Fixed wLength validation on EP0 vendor requests (issue #29)
- Fixed debug-over-USB race condition with ring buffer (issue #26)
- Consolidated three ConfGPIO functions into parameterized ConfGPIOSimple

### Phase 2: License Cleanup
- R82xx tuner driver removed; clean MIT + Cypress SLA licensing posture
- See `docs/LICENSE_ANALYSIS.md` for full analysis

### Phase 2.5: Documentation
- Wrote `docs/architecture.md`, `docs/debugging.md`,
  `docs/diagnostics_side_channel.md`, `docs/wedge_detection.md`

---

## Phase 3: Code Cleanup & Consolidation

Eight steps, ordered by impact-to-risk ratio. Each step must pass
`make clean && make all` before proceeding to the next.

---

### Step 1: Extract GpioShiftOut helper in rx888r2.c

**Problem:** `rx888r2_SetAttenuator()` (lines 59-76) and `rx888r2_SetGain()`
(lines 82-99) contain identical bit-bang SPI loops differing only in bit count,
mask, and latch pin.

**Change:**
- Add `static void GpioShiftOut(uint8_t latch_pin, uint8_t value, uint8_t bits)`
  that contains the shared clock/data/latch loop
- Rewrite both functions as thin wrappers calling `GpioShiftOut`

**Files:** `SDDC_FX3/radio/rx888r2.c`

---

### Step 2: Fix misleading and stale comments

| Location | Problem | Fix |
|----------|---------|-----|
| `RunApplication.c:71,92` | ConfGPIO wrappers say "output" for input configs | Change to "input" / "input with pull-up" |
| `Support.c:179` | CheckStatusSilent says "displays and stall" | Rewrite to match actual blink-and-reset behavior |
| `i2cmodule.c:26` | Says "100KHz" bitrate | Change to "400KHz" to match `I2C_BITRATE` |
| `StartUP.c:44` | Says "GPIF can be '100MHz'" | Change to "~201MHz" (403MHz / clkDiv=2) |
| `Si5351.c:109` | References nonexistent "si5351a.h header file" | Remove or update reference |
| `docs/debugging.md:167` | MsgParsing label 2 says "free" | Update to describe PIB error info |
| `rx888r2.c:87` | `SetGain` comment says "ATT_LE latched" | Fix to "VGA_LE" (copy-paste from SetAttenuator) |

**Files:** 7 files as listed above

---

### Step 3: Remove dead code

| Location | What to remove |
|----------|---------------|
| `StartStopApplication.c:45-49` | Commented-out `#define TH1_BUSY/TH1_WAIT/TH0_BUSY` block |
| `i2cmodule.h:27` | Commented-out `SendI2cbytes` declaration |
| `i2cmodule.h:9` | Unused `#define I2C_ACTIVE` macro |
| `i2cmodule.c:50-51` | Commented-out `DebugPrint` call |
| `RunApplication.c:224` | Commented-out `for (Count = 0; ...)` loop |
| `USBhandler.c:382` | Commented-out `DebugPrint` in LPMRequest_Callback |

**Files:** 4 files as listed above

---

### Step 4: Rename inconsistent functions to PascalCase

| Current name | New name | File(s) |
|-------------|----------|---------|
| `Pib_error_cb` | `PibErrorCallback` | StartStopApplication.c |
| `setupPLL` | `SetupPLL` | driver/Si5351.c (static) |
| `setupMultisynth` | `SetupMultisynth` | driver/Si5351.c (static) |
| `Si5351init` | `Si5351Init` | driver/Si5351.c, driver/Si5351.h, RunApplication.c |
| `USBEvent_Callback` | `USBEventCallback` | USBhandler.c, StartUP.c |
| `LPMRequest_Callback` | `LPMRequestCallback` | USBhandler.c, StartUP.c |

The `rx888r2_*` prefix is intentional namespacing for the radio driver —
leave those as-is.

**Files:** 5 files

---

### Step 5: Move duplicate defines to protocol.h

**Problem:** `FX3_CMD_BASE`, `FX3_CMD_COUNT`, and `SETARGFX3_LIST_COUNT` are
defined identically in both `DebugConsole.c` and `USBhandler.c`.

**Change:**
- Move all three defines into `protocol.h` (single source of truth)
- Remove the duplicates from `DebugConsole.c` and `USBhandler.c`
- Move `FX3CommandName[]` and `SETARGFX3List[]` arrays from `DebugConsole.c`
  to `USBhandler.c` (where they're consumed via extern) or to a shared location

**Also:** Fix `i2cmodule.h` circular include — it includes `Application.h`
which includes `i2cmodule.h`. Change `i2cmodule.h` to include only
`cyu3types.h` (the only Cypress type it actually needs).

**Files:** `protocol.h`, `DebugConsole.c`, `USBhandler.c`, `i2cmodule.h`

---

### Step 6: Consolidate GPIO LED setup (StartUP.c + Support.c)

**Problem:** `IndicateError()` in `StartUP.c:17-33` and `ErrorBlinkAndReset()`
in `Support.c:27-50` both configure `GPIO_LED_BLUE_PIN` with identical
`CyU3PGpioSimpleConfig_t` initialization.

**Change:**
- Extract a shared `ConfigureLedGpio()` helper (or have `IndicateError` call
  `ErrorBlinkAndReset` directly if appropriate)

**Files:** `StartUP.c`, `Support.c`

---

### Step 7: Rename files via git mv + makefile update

| Current | Renamed |
|---------|---------|
| `USBdescriptor.c` | `USBDescriptor.c` |
| `USBhandler.c` | `USBHandler.c` |
| `StartUP.c` | `StartUp.c` |

**Change:**
- `git mv` each file
- Update `SDDC_FX3/makefile` source lists
- Update any `#include` directives that reference these filenames (if any)

**Risk:** Low — file renames are purely cosmetic. Git tracks renames cleanly.

**Files:** 3 source files, `makefile`

---

### Step 8: Add `gl` prefix to unprefixed globals

**Problem:** Many globals lack the Cypress-convention `gl` prefix.

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

**Risk:** Highest of all steps — touches many `extern` declarations across
multiple files. Each rename must be done globally (definition + all references).
Do one variable at a time, build-test between each.

**Files:** All `.c` and `.h` files under `SDDC_FX3/`

---

## Phase 4: Hardware Test Tools

(Retained from prior plan — to be implemented after Phase 3.)

### Test Tool #1: `fx3_cmd` — Vendor Command Exerciser

A small C program using libusb-1.0 that sends individual vendor commands and
reports success/failure. See `tests/fx3_cmd.c`.

### Test Tool #2: `fw_test.sh` — Automated Firmware Test Script

A shell script using `rx888_stream` for firmware upload and streaming tests,
and `fx3_cmd` for individual command tests. Outputs TAP format.

Build and dependency requirements: `libusb-1.0-0-dev`, `rx888_tools` as git
submodule at `tests/rx888_tools/`.

---

## Verification Protocol

After each step:
1. `make clean && make all` — must produce `SDDC_FX3.img` with zero errors
2. `arm-none-eabi-size SDDC_FX3.elf` — track text/data/bss changes
3. `git diff --stat` — review scope of changes before committing
