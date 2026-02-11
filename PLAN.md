# SDDC_FX3 Firmware — Project Plan

## Completed Work

The following was completed in the initial cleanup phase:

- Removed all non-RX888r2 radio variants (HF103, BBRF103, RX888v1, RX888r3,
  RX999, RXLUCY) and their drivers (ADF4351, RD5815, PCAL6408A)
- Simplified dispatch in RunApplication.c and USBhandler.c to RX888r2-only
- Internalized Interface.h into a self-contained protocol.h
- Cleaned up top-level orphaned files (stale .c copies, SDDC_FX3.h blob)
- Build verified: `make clean && make all` produces SDDC_FX3.img cleanly

---

## Phase 2: License Cleanup & Continued Project Hygiene

### Step 1: Remove GPL-licensed R82xx Tuner Driver

**Problem:** The R82xx tuner driver (`tuner_r82xx.c`, `tuner_r82xx.h`) is
licensed under GPL v2+, which is incompatible with the proprietary Cypress SDK
that gets linked into the same binary. Removing the tuner capability eliminates
this conflict. See `docs/LICENSE_ANALYSIS.md` for the full analysis.

**Key finding:** The R82xx driver is NOT needed for hardware detection. The
detection code in `RunApplication.c` only does a raw I2C probe at address
`0x74` — it never calls any R82xx driver function.

#### Files to delete:
- `SDDC_FX3/driver/tuner_r82xx.c`
- `SDDC_FX3/driver/tuner_r82xx.h`

#### SDDC_FX3/makefile:
- Remove `tuner_r82xx.c` from `DRIVERSRC`

#### SDDC_FX3/RunApplication.c:
- Remove `#include "tuner_r82xx.h"`
- Add local define: `#define R828D_I2C_ADDR 0x74` (for HW detection only)
- Hardware detection logic is otherwise unchanged

#### SDDC_FX3/USBhandler.c:
- Remove `#include "tuner_r82xx.h"`
- Remove global tuner state (`struct r82xx_priv tuner`, `struct r82xx_config
  tuner_config`, extern declarations for gain functions)
- Remove `r820_initialize()` function
- Remove `TUNERINIT` (0xB4) command handler
- Remove `TUNERTUNE` (0xB5) command handler
- Remove `TUNERSTDBY` (0xB8) command handler
- In `SETARGFX3` (0xB6): remove cases for `R82XX_ATTENUATOR` (1),
  `R82XX_VGA` (2), `R82XX_SIDEBAND` (3). **Keep** `DAT31_ATT` (10) and
  `AD8340_VGA` (11)

#### SDDC_FX3/protocol.h:
- Remove from `FX3Command` enum: `TUNERINIT`, `TUNERTUNE`, `TUNERSTDBY`
- Remove from `ArgumentList` enum: `R82XX_ATTENUATOR`, `R82XX_VGA`,
  `R82XX_SIDEBAND`, `R82XX_HARMONIC`
- **Keep** `SETARGFX3` (still used for DAT31_ATT and AD8340_VGA)
- **Keep** `DAT31_ATT` and `AD8340_VGA`

#### Compatibility:
- An older host driver sending tuner commands will receive USB STALL
  (`isHandled = CyFalse`), which is safe — no crash, just a failed request
- Host-side software should be updated to stop sending tuner commands

#### Build test:
`make clean && make all` must succeed.

---

### Step 2: Build Hardware Test Tools

**Goal:** Create test tools in `tests/` that can exercise every firmware
vendor command over USB, plus validate streaming performance. These tools
run on the Linux host against real RX888mk2 hardware.

**Reference codebase:**
[rx888_tools](https://github.com/ringof/rx888_tools) — specifically
`rx888_stream.c` — is a proven host-side streamer that works with the
stock firmware. We borrow its USB control transfer patterns and protocol
constants.

#### Analysis of rx888_stream vs. Firmware Protocol

rx888_stream's initialization sequence (in order):

| # | Host call | FX3 command | Firmware handler |
|---|-----------|-------------|-----------------|
| 1 | `rx888_stop_stream()` | STOPFX3 (0xAB) | Stop GPIF, flush EP — OK |
| 2 | `rx888_gpio(gpio_bits)` | GPIOFX3 (0xAD) | `rx888r2_GpioSet()` — OK |
| 3 | `rx888_set_arg(DAT31_ATT, att)` | SETARGFX3 (0xB6), wIndex=10 | `rx888r2_SetAttenuator()` — OK |
| 4 | `rx888_set_arg(AD8340_VGA, gain)` | SETARGFX3 (0xB6), wIndex=11 | `rx888r2_SetGain()` — OK |
| 5 | `rx888_cmd_u32(STARTADC, rate)` | STARTADC (0xB2) | `si5351aSetFrequencyA()` — OK |
| 6 | `rx888_start_stream()` | STARTFX3 (0xAA) | Start GPIF/DMA — OK |
| 7 | `rx888_cmd_u32(TUNERSTDBY, 0)` | TUNERSTDBY (0xB8) | See note below |

**Compatibility finding — TUNERSTDBY after R82xx removal:**
rx888_stream sends TUNERSTDBY as a best-effort "put VHF tuner to sleep"
after starting the HF stream. In the current firmware this calls
`r82xx_standby()` + `si5351aSetFrequencyB(0)`. After R82xx removal,
this command will hit the `default:` case and return USB STALL.
rx888_stream treats this as non-fatal (the comment says "best-effort")
so HF streaming is unaffected. The `si5351aSetFrequencyB(0)` side-effect
(turning off the tuner reference clock) is irrelevant without a tuner.

**Other commands not used by rx888_stream but present in firmware:**

| Command | Code | Notes |
|---------|------|-------|
| TESTFX3 | 0xAC | Device identification (returns HWconfig, FW version). Used by other host tools. |
| I2CWFX3 | 0xAE | Raw I2C write. Useful for diagnostics. |
| I2CRFX3 | 0xAF | Raw I2C read. Useful for diagnostics. |
| RESETFX3 | 0xB1 | Reboot FX3 to bootloader. |
| READINFODEBUG | 0xBA | Debug console over USB. |
| TUNERINIT | 0xB4 | R82xx init — will be removed. |
| TUNERTUNE | 0xB5 | R82xx tune — will be removed. |

#### Firmware Upload Requirement

The FX3 boots into bootloader mode (PID `0x00F3`) on every power cycle.
Firmware must be uploaded before any vendor commands work.  `fx3_cmd` does
NOT handle firmware upload — it assumes the device is already running at
app PID `0x00F1`.

`rx888_stream` (from [rx888_tools](https://github.com/ringof/rx888_tools))
handles this: its `-f` flag uploads a `.img` file to the bootloader and
waits for re-enumeration.  The `fw_test.sh` script uses `rx888_stream`
for this purpose as its first step.

#### Test Tool #1: `fx3_cmd` — Vendor Command Exerciser

A small C program using libusb-1.0 that sends individual vendor commands
and reports success/failure. Borrows the `ctrl_write_u32()` and
`ctrl_write_buf()` patterns from rx888_stream.

**Subcommands:**
```
fx3_cmd test              # TESTFX3: read HWconfig + FW version
fx3_cmd gpio <bits>       # GPIOFX3: set GPIO word
fx3_cmd adc <freq_hz>     # STARTADC: set ADC clock
fx3_cmd att <0-63>        # SETARGFX3/DAT31_ATT
fx3_cmd vga <0-255>       # SETARGFX3/AD8340_VGA
fx3_cmd start             # STARTFX3: start streaming
fx3_cmd stop              # STOPFX3: stop streaming
fx3_cmd i2cr <addr> <reg> <len>   # I2CRFX3: read I2C
fx3_cmd i2cw <addr> <reg> <data>  # I2CWFX3: write I2C
fx3_cmd reset             # RESETFX3: reboot to bootloader
fx3_cmd raw <code>        # Send raw vendor request (for stale-cmd tests)
```

Each subcommand prints a one-line PASS/FAIL result with the USB status.

**Files:**
- `tests/fx3_cmd.c` — single-file C program
- `tests/Makefile` — builds `fx3_cmd`, links against libusb-1.0

**Dependencies:** `libusb-1.0-0-dev` (same as rx888_stream)

#### Test Tool #2: `fw_test.sh` — Automated Firmware Test Script

A shell script that uses `rx888_stream` for firmware upload and streaming
tests, and `fx3_cmd` for individual command tests:

```
tests/fw_test.sh --firmware path/to/SDDC_FX3.img [options]
```

**Test sequence (15 tests):**
1. **Firmware upload** — `rx888_stream -f SDDC_FX3.img` uploads firmware;
   verify device appears at PID `0x00F1` via `lsusb`
2. **Device probe** — `fx3_cmd test`: verify HWconfig == 0x04 (RX888r2),
   firmware version matches expected
3. **GPIO test** — `fx3_cmd gpio`: set known pattern (LEDs on)
4. **ADC clock test** — `fx3_cmd adc 64000000`: set 64 MHz clock, verify ACK
5. **Attenuator spot-check** — `fx3_cmd att 0` and `fx3_cmd att 63`
6. **VGA spot-check** — `fx3_cmd vga 0` and `fx3_cmd vga 255`
7. **Stop** — `fx3_cmd stop`: ensure clean state
8. **Stale command tests** (post-R82xx removal) — `fx3_cmd raw 0xB4`,
   `0xB5`, `0xB8`: verify each returns STALL without crashing the device
9. **Streaming test** — run `rx888_stream` for N seconds, capture to
   file, verify:
   - Data was received (non-zero byte count)
   - Byte count matches expected rate (within 50%)
   - Data is not all-zero (ADC is actually sampling)

**Output:** TAP format (Test Anything Protocol) for easy integration
with CI or manual review.

#### Build and Dependency Requirements

[rx888_tools](https://github.com/ringof/rx888_tools) is included as a
git submodule at `tests/rx888_tools/`.  The test Makefile builds
`rx888_stream` from source (it needs `ezusb.c` for firmware upload)
and symlinks it into `tests/`.

```
# On the test host (Linux machine with RX888mk2 connected)
sudo apt install libusb-1.0-0-dev

# Initialize submodule and build everything
git submodule update --init
cd tests && make

# Run
./fw_test.sh --firmware ../SDDC_FX3/SDDC_FX3.img
```

### Step 3: (reserved — further cleanup TBD)

---

## Post-Cleanup License Summary

After completing Step 1, the firmware binary contains only:

| Component | License | Distribution |
|-----------|---------|-------------|
| Application code | MIT (Oscar Steila 2017-2020, David Goncalves 2024-2026) | Freely distributable |
| Cypress FX3 SDK | Cypress SLA | Object code distributable royalty-free with Cypress IC products (§1.3) |
| ThreadX RTOS | Sublicensed via Cypress SLA §6.2 | Covered under SDK distribution rights |

No copyleft obligations. Clean licensing posture.

---

## Verification Protocol

After each step:
1. `make clean && make all` — must produce `SDDC_FX3.img` with zero errors
2. `arm-none-eabi-size SDDC_FX3.elf` — track text/data/bss changes
3. Flash to RX888mk2 and run `tests/fw_test.sh` for automated validation
4. Manual spot-check: run `rx888_stream` pipeline, verify signal in SDR app
