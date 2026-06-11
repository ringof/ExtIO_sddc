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

2. **Tuner reference** — program Si5351 **CLK2 (CLKB)** to the R828D reference
   over `I2CWFX3`, exactly as you program CLK0 for the ADC. Disable it in HF
   mode to keep its spurs out of the ADC.

3. **Tune the R828D** — over `I2CWFX3` / `I2CRFX3` at `0x74`. Probe first: read
   reg `0x00`, expect `0x69`. Then init + `set_freq`. Wire format: **`wValue` =
   device addr, `wIndex` = register, `wLength` = byte count**, data in the EP0
   payload.

4. **Stream and look** — `STARTFX3`, FFT the IF. With the chip's 8 MHz filter,
   `LO = RF + IF` and the carrier lands at **IF ≈ 4.57 MHz** in the ADC
   spectrum. That's the proof.

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
| R828D addr / ID | `0x74` / reg `0x00` == `0x69` |
| I2C passthrough | `wValue`=addr, `wIndex`=reg, `wLength`=count |
| Tuner reference | Si5351 **CLK2 / CLKB** |
| IF center (8 MHz filter) | **4.57 MHz** (`LO = RF + IF`) |

No firmware build, no new commands. Lone hardware question (5-min bench): do the
front-end attenuator/VGA sit in the common path, so they also condition the
tuner IF?
