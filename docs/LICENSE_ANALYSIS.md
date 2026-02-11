# License Analysis & GPL Removal Plan

## 1. Current License Landscape

The SDDC_FX3 firmware binary is composed of code from four distinct license regimes:

| Component | License | Copyright | Files |
|-----------|---------|-----------|-------|
| SDDC_FX3 application code | MIT | Oscar Steila 2017-2020, David Goncalves 2024-2026 | `*.c`, `*.h` in `SDDC_FX3/` (excluding drivers) |
| Cypress FX3 SDK | Proprietary (Cypress SLA) | Cypress Semiconductor 2010-2018 | `SDK/fw_lib/`, `SDK/fw_build/`, `cyfxtx.c`, `cyfx_gcc_startup.S`, `makefile` |
| ThreadX RTOS | Proprietary (Express Logic), sublicensed via Cypress SLA | Express Logic Inc. 1996-2007 | Embedded in SDK `.a` libraries; headers in `SDK/fw_lib/1_3_4/inc/tx_*.h` |
| R820T/R828D tuner driver | **GPL v2+** | Mauro Carvalho Chehab, Steve Markgraf 2013 | `SDDC_FX3/driver/tuner_r82xx.c`, `tuner_r82xx.h` |

## 2. The GPL v2 Problem

### What GPL v2 requires

GPL v2 is a strong copyleft license. When GPL code is statically linked into a
binary, the GPL requires the **entire combined work** to be distributed under
GPL-compatible terms (Section 2b).

### Why this is a conflict

The Cypress SDK license is GPL-incompatible:

- **Distribution restricted to object code only** (Cypress SLA §1.3) — GPL
  requires source availability
- **Must incorporate a Cypress IC** (Cypress SLA §1.1) — GPL prohibits
  additional restrictions on recipients
- **Non-transferable license** (Cypress SLA §1.1) — GPL requires sublicensing
  to all downstream recipients
- **Confidentiality obligations** (Cypress SLA §1.4, §5) — GPL requires public
  source availability

The ThreadX RTOS (linked via Cypress `.a` libraries) adds a second proprietary
layer in the same binary.

### Practical reality

Strictly interpreted, no single license can cleanly cover the combined binary.
This same tension exists in virtually all FX3-based open-source SDR projects.
Common defenses include the "system library exception" (GPL v2 §3 exempts
"major components of the operating system on which the executable runs"), but
this is debatable for embedded firmware.

### Cypress SDK redistribution

The Cypress SLA §1.4 and §5 require keeping SDK source confidential. However,
Cypress/Infineon distributes the SDK publicly via their website and numerous
reference projects include it in public repositories. The confidentiality clause
is arguably moot for materials Cypress themselves have made public.

## 3. Recommendation: Remove GPL Code

The cleanest resolution is to **remove the GPL-licensed R82xx tuner driver
entirely**, eliminating the copyleft conflict. The resulting binary would be:

- **MIT** (application code) — permissive, no conflict
- **Cypress SLA** (SDK) — proprietary but permits object-code distribution
  with products using Cypress ICs
- **ThreadX** — sublicensed through Cypress SLA §6.2

This is a clean, unambiguous licensing posture.

## 4. R82xx Usage Analysis

The R82xx driver is used in exactly two ways:

### 4a. Hardware detection (RunApplication.c:168-192)

During startup, `ApplicationThread()` probes I2C address `0x74` (the R828D
default address) to detect whether the tuner IC is present. Combined with a
GPIO36 sense check, this determines `HWconfig = RX888r2`.

**This does NOT require the R82xx driver.** It only needs:
- The I2C address constant (`0x74`)
- A raw I2C probe via `I2cTransfer()`

Both are already available without `tuner_r82xx.h`.

### 4b. Tuner control (USBhandler.c:109-312)

Four USB vendor commands dispatch to R82xx functions:

| USB Command | Code | R82xx Function | Purpose |
|-------------|------|----------------|---------|
| `TUNERINIT` | 0xB4 | `r820_initialize()` → `r82xx_init()`, `r82xx_set_bandwidth()` | Initialize tuner |
| `TUNERTUNE` | 0xB5 | `r82xx_set_freq64()` | Set RF frequency |
| `TUNERSTDBY`| 0xB8 | `r82xx_standby()` | Tuner standby |
| `SETARGFX3` | 0xB6 | `set_all_gains()`, `set_vga_gain()`, `r82xx_set_sideband()` (wIndex 1-3) | Gain/sideband control |

The `SETARGFX3` command also handles non-tuner arguments (`DAT31_ATT=10`,
`AD8340_VGA=11`) which must be preserved.

## 5. Removal Plan

### Step 1: Remove tuner driver source files

Delete:
- `SDDC_FX3/driver/tuner_r82xx.c`
- `SDDC_FX3/driver/tuner_r82xx.h`

### Step 2: Update makefile

In `SDDC_FX3/makefile`, remove `tuner_r82xx.c` from `DRIVERSRC`:

```makefile
# Before:
DRIVERSRC=\
    Si5351.c        \
    tuner_r82xx.c

# After:
DRIVERSRC=\
    Si5351.c
```

### Step 3: Update RunApplication.c

Remove `#include "tuner_r82xx.h"` (line 14). The hardware detection code at
lines 168-192 only uses `R828D_I2C_ADDR` from the tuner header. Replace with
a local constant:

```c
#define R828D_I2C_ADDR 0x74   /* R828D tuner I2C address, used for HW detection only */
```

The rest of the detection code (`I2cTransfer`, `GPIO36` check, `HWconfig`
assignment) is unchanged — it never calls any R82xx driver functions.

### Step 4: Update USBhandler.c

Remove `#include "tuner_r82xx.h"` (line 15) and all tuner-related code:

1. **Remove global tuner state** (lines 40-46):
   - `struct r82xx_priv tuner`
   - `struct r82xx_config tuner_config`
   - `extern` declarations for `set_all_gains`, `set_vga_gain`

2. **Remove `r820_initialize()` function** (lines 109-131)

3. **Remove/stub USB command handlers:**
   - `TUNERINIT` (0xB4, lines 253-264) — remove entirely or return STALL
   - `TUNERTUNE` (0xB5, lines 275-283) — remove entirely or return STALL
   - `TUNERSTDBY` (0xB8, lines 266-273) — remove entirely or return STALL
   - `SETARGFX3` (0xB6, lines 285-312) — remove R82xx cases (wIndex 1-3),
     **keep** `DAT31_ATT` (10) and `AD8340_VGA` (11)

### Step 5: Update protocol.h

Remove tuner-specific command codes and argument enums:

1. **Remove from `FX3Command` enum:**
   - `TUNERINIT = 0xB4`
   - `TUNERTUNE = 0xB5`
   - `TUNERSTDBY = 0xB8`
   - Update `SETARGFX3` comment (no longer R8xx-related)

2. **Remove from `ArgumentList` enum:**
   - `R82XX_ATTENUATOR = 1`
   - `R82XX_VGA = 2`
   - `R82XX_SIDEBAND = 3`
   - `R82XX_HARMONIC = 4`
   - **Keep** `DAT31_ATT = 10` and `AD8340_VGA = 11`

### Step 6: Verify build

Confirm the firmware compiles and links cleanly with the ARM GCC toolchain
after removal.

### Step 7: Host-side compatibility note

The host-side ExtIO driver (not in this repo) will need corresponding updates
to stop sending `TUNERINIT`/`TUNERTUNE`/`TUNERSTDBY` commands and the R82xx
`SETARGFX3` sub-commands. The firmware should handle unknown commands gracefully
(return STALL / `isHandled = CyFalse`) so that an older host driver against
new firmware degrades safely rather than crashing.

## 6. Post-Removal License Summary

After removing the R82xx driver:

| Component | License | Distribution Rights |
|-----------|---------|-------------------|
| Application code | MIT | Freely distributable in any form |
| Cypress FX3 SDK | Cypress SLA | Object code distributable royalty-free with Cypress IC products (§1.3) |
| ThreadX RTOS | Sublicensed via Cypress SLA §6.2 | Covered under SDK distribution rights |

The final `.img` binary can be distributed under MIT for the application code,
with the SDK components covered by the Cypress SLA's distribution grant. No
copyleft obligations remain.
