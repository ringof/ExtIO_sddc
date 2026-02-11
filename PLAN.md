# Plan: Extricate RX888mk2 Firmware from Multi-Radio Codebase

## Goal
Strip the SDDC_FX3 firmware down to RX888mk2-only (aka rx888r2), removing all
other hardware variants and unused driver code. Produce a firmware that builds
cleanly and is byte-for-byte functionally identical for the RX888r2 hardware
path, so it can be tested on real hardware between steps.

## Baseline
- Build verified: `make all` produces `SDDC_FX3.img` (146KB) with zero errors
- Toolchain: `arm-none-eabi-gcc 13.2.1` + FX3 SDK 1.3.4
- Code size: 136KB text (76% of 180KB code region)

## What RX888r2 Actually Uses
From the dispatch in `USBhandler.c` and `RunApplication.c`, the rx888r2 path calls:
- `rx888r2_GpioInitialize()`, `rx888r2_GpioSet()`, `rx888r2_SetAttenuator()`, `rx888r2_SetGain()`
- `Si5351` (clock gen): `Si5351init()`, `si5351aSetFrequencyA()`, `si5351aSetFrequencyB()`
- `tuner_r82xx` (R828D tuner): `r820_initialize()`, `r82xx_set_freq64()`, `r82xx_standby()`, `set_all_gains()`, `set_vga_gain()`, `r82xx_set_sideband()`
- `I2cTransfer()` for I2C bus operations
- All the USB/GPIF/DMA infrastructure

## What RX888r2 Does NOT Use
| Module | Linked Size | Used By |
|--------|-------------|---------|
| `adf4351.c` | 6.3KB | RX999, RXLUCY only |
| `rd5815.c` | 7.8KB | RX888r3 only |
| `pcal6408a.c` | 1.5KB | RXLUCY only |
| `rx999.c` | 5.7KB | RX999 only |
| `rx888r3.c` | 3.8KB | RX888r3 only |
| `rxlucy.c` | 3.7KB | RXLUCY only |
| `hf103.c` | 2.5KB | HF103 only |
| `bbrf103.c` | 2.1KB | BBRF103 only |
| `rx888.c` | 2.1KB | RX888 (v1) only |
| **Total removable** | **~35KB** | |

Note: despite `--gc-sections`, all of these are linked into the final image
because the switch/case dispatch in `USBhandler.c` and `RunApplication.c`
references every variant's functions.

---

## Execution Steps

Each step produces a buildable, testable firmware. After each step you can
pull the branch and flash the .img to verify it on hardware.

### Step 1: Remove other radio source files and drivers from the build

**Files to delete:**
- `radio/bbrf103.c`, `radio/hf103.c`, `radio/rx888.c`
- `radio/rx888r3.c`, `radio/rx999.c`, `radio/rxlucy.c`
- `driver/adf4351.c`, `driver/adf4351.h`
- `driver/rd5815.c`, `driver/rd5815.h`
- `pcal6408a.c`, `pcal6408a.h`

**Files to edit:**
- `makefile`: Remove deleted files from `RADIOSRC`, `DRIVERSRC`
- `radio/radio.h`: Remove declarations for deleted radio modules
- `Application.h`: Remove `#include "adf4351.h"`, remove declarations for
  deleted radio functions, remove `extern adf4350_init_param adf4351_init_params`

**Build test:** `make clean && make all` must succeed.

**Expected result:** Same functional firmware for RX888r2, but ~35KB smaller
image because the dead code switch cases now reference nothing.

### Step 2: Simplify dispatch to RX888r2-only

**`RunApplication.c` changes:**
- Remove the multi-stage hardware detection cascade (Si5351 fail → HF103/RXLUCY,
  R820T probe → BBRF103/RX888, R828D probe → RX888r2, RD5815 → RX888r3,
  fallback → RX999)
- Replace with: init Si5351, probe R828D, if found set `HWconfig = RX888r2`,
  else set `HWconfig = NORADIO` and log error
- Remove the `switch(HWconfig)` for GpioInitialize — just call
  `rx888r2_GpioInitialize()` directly
- Remove GPIO50/GPIO45/GPIO52/GPIO53 sense pin setup (only used by other variants)

**`USBhandler.c` changes:**
- `GPIOFX3`: Remove switch, call `rx888r2_GpioSet()` directly
- `TUNERINIT`: Remove switch, call `r820_initialize()` directly
- `TUNERSTDBY`: Remove switch, call `r82xx_standby()` + `si5351aSetFrequencyB(0)` directly
- `TUNERTUNE`: Remove switch, call `r82xx_set_freq64()` directly
- `SETARGFX3`/`DAT31_ATT`: Remove switch, call `rx888r2_SetAttenuator()` directly
- `SETARGFX3`/`AD8340_VGA`: Remove switch, call `rx888r2_SetGain()` directly
- `SETARGFX3`/`PRESELECTOR`: Remove entirely (RX888r2 has no preselector)
- `SETARGFX3`/`VHF_ATTENUATOR`: Remove entirely (RX888r2 has no VHF attenuator)

**`USBdescriptor.c` changes:**
- Remove product string descriptors for other hardware variants
- Remove the `switch(HWconfig)` in `SetUSBdescriptors()`, just set the RX888r2
  product string

**`Application.h` changes:**
- Remove all radio function declarations except rx888r2
- Remove detect GPIO defines for other variants (GPIO50, GPIO52, GPIO53, GPIO45)

**Build test:** `make clean && make all` must succeed.
**Hardware test:** Flash and verify on RX888mk2. All host software operations
(tune, gain, attenuator, start/stop streaming) should work identically.

### Step 3: Internalize Interface.h

**Current state:** `Application.h` includes `../Interface.h`, the only file
shared with the (dead) host application code.

**Change:** Copy the relevant definitions from `Interface.h` directly into
`Application.h` (or a new `protocol.h` inside `SDDC_FX3/`). Remove the
`#include "../Interface.h"` reference. This makes the firmware directory
fully self-contained.

Trim to only what the firmware uses:
- `FX3Command` enum (keep all — firmware responds to all of these)
- `GPIOPin` enum (keep only the pins RX888r2 uses: SHDWN, DITH, RANDO,
  BIAS_HF, BIAS_VHF, LED_YELLOW, LED_RED, LED_BLUE, PGA_EN, VHF_EN)
- `RadioModel` enum (keep only NORADIO and RX888r2, or keep all for
  backward-compatible TESTFX3 response)
- `ArgumentList` enum (keep R82XX_ATTENUATOR, R82XX_VGA, R82XX_SIDEBAND,
  DAT31_ATT, AD8340_VGA. Remove PRESELECTOR, VHF_ATTENUATOR, R82XX_HARMONIC)
- `FIRMWARE_VER_*` (keep)
- `_DEBUG_USB_` / `MAXLEN_D_USB` (keep)

**Build test:** `make clean && make all` must succeed.

### Step 4: Clean up top-level junk

**Files to delete from repo root (not part of firmware, not used by anything):**
- `SDDC_FX3.h` — orphaned 738KB firmware binary blob, nothing includes it
- `DebugConsole.c` — stale copy of SDDC_FX3/DebugConsole.c at repo root
- `USBhandler.c` — stale copy at repo root
- `StartStopApplication.c` — stale copy at repo root
- `HWSDRtable.h` — documentation file, not used in any build

These are not part of the firmware build (the makefile never references
parent directory `.c` files), but removing them eliminates confusion.

**Build test:** `make clean && make all` must succeed (trivially — these
files were never compiled).

---

## What Is NOT Changed (and Why)

- **`cyfxtx.c`** — FX3 RTOS glue, heap management, exception handlers.
  Required as-is.
- **`DebugConsole.c`** — Debug infrastructure. Useful for development.
- **`Support.c`** — Utility functions used throughout. Keep.
- **`StartStopApplication.c`** — USB endpoint/DMA setup. Hardware-independent.
- **`StartUP.c`** — CPU init. Hardware-independent.
- **`i2cmodule.c`** — I2C bus driver. Used by Si5351, R82xx, hardware detect.
- **`Si5351.c`** — Clock generator. Used by RX888r2 for ADC clock.
- **`tuner_r82xx.c`** — R828D tuner driver. Core to RX888r2 VHF path.
- **`SDDC_GPIF.h`** — Auto-generated GPIF state machine. Do not hand-edit.
- **`SDK/`** — Vendor SDK. Do not modify.

---

## Verification Protocol

After each step:
1. `make clean && make all` — must produce `SDDC_FX3.img` with zero errors
2. `arm-none-eabi-size SDDC_FX3.elf` — track text/data/bss shrinkage
3. (User) Flash to RX888mk2 and test: USB enumeration, TESTFX3 response,
   start/stop ADC streaming, tuner init/tune/standby, attenuator, VGA gain
