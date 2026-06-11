# Making VHF Work on the RX888 mk2 — host-side bring-up (for Phil)

**The firmware needs no changes.** VHF is configured entirely through EP0 vendor
commands you already use. Tuner config rides **EP0**, the sample stream rides
**EP1**, so a config utility can run **concurrently** with the streamer — open
the device, fire control transfers, no interface claim needed (vendor /
device-recipient; proven by the `two_actor_open` test, #143). Configure or
retune live, in a separate process, while ka9q-radio streams.

## You already have every primitive

| Need | Command |
|---|---|
| HF/VHF switch | `GPIOFX3` |
| Tuner reference clock (Si5351 CLKB / CLK2) | `I2CWFX3` to Si5351 (as you do CLK0) |
| R828D tune / gain / sideband | `I2CWFX3` / `I2CRFX3` to `0x74` |
| Front-end attenuator / VGA | `SETARGFX3` (already in use) |
| Stream + stats | `STARTFX3` / `STOPFX3` / `GETSTATS` |

## The four things to do

1. **Select VHF** — `GPIOFX3`, a 32-bit LE word in the **data phase**
   (`wLength = 4`). Your HF word with `VHF_EN` (bit 15, `0x8000`) **set**. It's
   a whole-word write (every steady-state pin applied each call), so start
   from your existing HF word and just flip the bits below — everything else
   (ADC settings, etc.) carries over untouched. Move bias `BIAS_HF`→`BIAS_VHF`
   only if you want the VHF-port bias-tee.

2. **Tuner reference** — program Si5351 **CLK2 (CLKB)** to **16 MHz**
   (`R828D_FREQ`, from the original host code), exactly as you program CLK0 for
   the ADC. Disable it in HF mode to keep its spurs out of the ADC.

3. **Tune the R828D** — over `I2CWFX3` / `I2CRFX3` at `0x74`. Probe first: read
   reg `0x00`, expect `0x69` (after the R82xx per-byte bit-reverse). Then init
   + `set_freq` with `LO = RF + 4.57 MHz`. Wire format: **`wValue` = device
   addr, `wIndex` = register, `wLength` = byte count**, data in the EP0 payload.

4. **Stream and look** — `STARTFX3`, FFT the IF. The carrier lands at
   **IF ≈ 4.57 MHz** (`R828D_IF_CARRIER`) in the ADC spectrum. That's the proof.

## Prerequisites (before the VHF front-end)

The VHF config above only touches the front-end. Two things must already be set:

1. **Firmware loaded** — if the device is in bootloader (`04B4:00F3`),
   `fx3_cmd load SDDC_FX3/SDDC_FX3.img`; it re-enumerates to `04B4:00F1`.
2. **ADC sample clock** — `STARTADC <rate>` (e.g. `fx3_cmd adc 64000000`). Sets
   Si5351 **CLKA** and the clock-enabled flag `STARTFX3` requires. Any normal
   HF rate works — the 4.57 MHz IF sits low in the band. (The Si5351 itself is
   initialized at boot, so CLKB programming works once firmware is up.)

The only hard ordering rule is `STARTADC` before `STARTFX3`; the VHF front-end
config can go anywhere in that window (it touches CLKB/PLL-B and the R828D,
independent of CLKA/PLL-A and the stream).

`vhf_tune.py` can run both prerequisites for you via flags — `--load IMG`
(delegates to `fx3_cmd load`) and `--adc RATE_HZ` (native `STARTADC`):

```
python3 vhf_tune.py 144000000 --load SDDC_FX3/SDDC_FX3.img --adc 64000000 --persist
fx3_cmd start            # STARTFX3, then capture/FFT
```

## Verify as you go

Each phase has a cheap read-back — don't fly blind:

- **Firmware alive** (beyond `lsusb`): `TESTFX3` returns
  `[hwconfig, fw_major, fw_minor, vendor_rqt_count]`; expect `hwconfig = 0x04`.
- **CLKB on**: read back Si5351 `CLK2_CONTROL` (reg 18); bit 7 clear = enabled.
- **Tuner reachable**: the reg `0x00` == `0x69` probe above — abort if it doesn't
  ACK before you tune.
- **PLL locked**: after `set_pll`, read reg `0x00`; `data[2] & 0x40` = lock.

`vhf_tune.py` (this repo) is a worked reference that does the whole lifecycle
— enter+init, tune to a frequency argument, standby on exit — with these four
checks wired in. The Si5351 CLKB=16 MHz and R828D init register sequences are
ported there verbatim from the firmware.

## Tuner code — use clean upstream, *not* the firmware copy

The firmware's `tuner_r82xx.c` is an **FX3-mangled fork** (Cypress types,
embedded sleeps) — treat it as *reference only*, not a porting base. Use
upstream **librtlsdr**:

- **`steve-m/librtlsdr` → `src/tuner_r82xx.c`** — canonical, maintained,
  host-native. Best base.
- Use the firmware copy + `tuner_r82xx_explained.md` only for **board
  specifics**: R828D at `0x74`; the reference is **Si5351-fed (CLKB), not a
  fixed crystal** — so set `cfg->xtal` = whatever you program CLK2 to; and the
  8 MHz → **4.57 MHz** IF.
- *(optional, later)* Linux `drivers/media/tuners/r820t.c` for **image-rejection
  (IMR) self-cal** — the chip self-calibrates via its own ring-osc + power
  detector, all over I2C; not in librtlsdr. Explainer §9.

For a first demo you don't need the whole driver — `init → set_mux → set_pll →
gain` is a few hundred lines.

## Quick reference

| Item | Value |
|---|---|
| VHF switch | `GPIOFX3`, `VHF_EN = 0x8000` set |
| `GPIOFX3` payload | 32-bit LE, `wLength = 4` |
| R828D addr / ID | `0x74` / reg `0x00` == `0x69` (bit-reversed) |
| I2C passthrough | `wValue`=addr, `wIndex`=reg, `wLength`=count |
| Tuner reference | Si5351 **CLK2 / CLKB** = **16 MHz** (`R828D_FREQ`) |
| IF center (8 MHz filter) | **4.57 MHz** (`R828D_IF_CARRIER`; `LO = RF + IF`) |
| Liveness | `TESTFX3` → `hwconfig 0x04`, fw, vendor-rqt count |

No firmware build, no new commands. The front-end attenuator/VGA *are* in the
common path — the original host code sets the AD8370 VGA (`SETARGFX3`, `0x83`)
in VHF mode, so they condition the tuner IF too; HF-only `DAT31_ATT` aside,
VHF gain = R828D-internal (I2C) + that VGA.
