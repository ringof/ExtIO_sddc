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

### Step 2: (reserved — further cleanup TBD)

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
3. (User) Flash to RX888mk2 and test core functionality
