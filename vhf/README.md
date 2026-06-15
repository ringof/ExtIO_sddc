# RX888 mk2 — VHF/UHF front end (R828D tuner)

Host-side control of the RX888 mk2's **R828D** VHF/UHF tuner, used as a
**downconverter** for a direct-sampling receiver (e.g. ka9q-radio): the tuner
slides a chunk of VHF/UHF spectrum down to a fixed IF that the ADC samples.

## Quick start (hear FM in ~4 commands)

**Prereqs:** RX888 mk2 plugged in (app-mode), `pip install pyusb textual`,
ka9q-radio image built (`docker build -t ka9q-radio docker/ka9q-radio/`).

```
./docker/ka9q-radio/ka9q.sh start --vhf           # 1. ADC streamer; parks a WBFM rx at 4.57 MHz
python3 vhf/vhf_fm_radio.py                        # 2. the tuner TUI — tune with the arrow keys
./docker/ka9q-radio/ka9q.sh monitor fm-pcm.local  # 3. audio out (needs /dev/snd)
```

**✅ It works when:** the TUI header shows `LOCKED` and you hear the station as
you tune with `← →`. Press `q` in the TUI to standby back to HF.

Full walkthrough, the CLI path, and troubleshooting: **`vhf_fm_howto.md`**. New to
the design? Read the next section first.

## What this is — and what the firmware does *not* do

The RX888 firmware does **no tuner control**. With respect to the tuner it is
purely:

- an **I2C bridge** to the R828D and Si5351 (`I2CWFX3` / `I2CRFX3` EP0 vendor
  commands),
- a **GPIO setter** (HF/VHF antenna switch, bias-tee), and
- an **ADC streamer** (it also inits the Si5351 and runs the GPIF→USB pipe).

The original GPL R82xx tuner driver was **removed** from the firmware. So the
tuner driver is **yours to implement on the host**, rtl-sdr style, over the I2C
passthrough — init the chip, program the PLL, configure the tracking filter, set
gains, tune. To make that tractable, this directory gives you a guide, a working
reference driver, and a runnable demo.

## Contents

| File | What it is |
|------|------------|
| `tuner_r82xx_explained.md` | **The guide** — how the R820T2/R828D works, register by register (signal path, PLL, IF filter, gains, standby, image rejection), with datasheet + reference-code links. |
| `rx888_vhf.py` | **The reference driver** — host-side `RX888` class: EP0 transport, Si5351 CLKB reference, R828D init/tune/standby, gains, read bit-order handling, shadow-register masked writes. |
| `vhf_tune.py` | CLI wrapper — tune the front end and hold, alongside a streamer. |
| `vhf_fm_radio.py` | **Runnable demo** — interactive FM-radio TUI built on the driver (gain/AGC, filter, IF-offset probe). |
| `vhf_fm_howto.md` | End-to-end HOWTO: VHF FM reception with ka9q-radio (the two-actor model). |

## The mental model

The Si5351 has two independent clock outputs, and that separation is the trick:

- **CLKA** (PLL-A) → ADC sample clock → owned by your **streamer/receiver** (radiod, etc.).
- **CLKB** (PLL-B) → R828D reference → owned by your **tuner control** (this driver).

The tuner mixes `LO = RF + 4.57 MHz`, so the station you tune to lands at
**4.570 MHz** in the ADC spectrum — park your receiver there. Tuner control (EP0)
and streaming (EP1 bulk) run **concurrently**; EP0 needs no interface claim.

## Run the demo

```
python vhf/vhf_fm_radio.py        # needs: pip install pyusb textual
```

Run your ADC streamer/receiver alongside it — that side owns the sample clock —
and park it at **4.57 MHz**. See `vhf_fm_howto.md` for the full ka9q-radio
walk-through.

## Firmware EP0 contract (what you build on)

Everything the host does goes through EP0 vendor requests. The tuner-relevant ones:

| Request | Code | Dir | Payload |
|---------|------|-----|---------|
| `I2CWFX3` | `0xAE` | OUT | `wValue` = device addr, `wIndex` = register, data = bytes to write |
| `I2CRFX3` | `0xAF` | IN  | `wValue` = device addr, `wIndex` = register, `wLength` = bytes to read |
| `GPIOFX3` | `0xAD` | OUT | 4-byte LE control word (HF/VHF switch, bias) |
| `STARTADC` | `0xB2` | OUT | 4-byte LE ADC sample rate |
| `STARTFX3` / `STOPFX3` | `0xAA` / `0xAB` | OUT | start/stop the GPIF→USB stream |
| `TESTFX3` | `0xAC` | IN | `[hwconfig, fw_hi, fw_lo, vendor_rqt_count]` |

(bmRequestType: OUT = `0x40`, IN = `0xC0` — vendor | device.)

**I2C addressing — the easy thing to get wrong.** Pass the **8-bit, write-form**
device address (LSB = 0); the firmware ORs in the R/W bit itself for the read
phase (it issues the repeated-START). So:

- R828D tuner → `0x74` (7-bit `0x3A` << 1)
- Si5351 clock → `0xC0` (7-bit `0x60` << 1)

Do **not** pass a 7-bit address and do **not** set the R/W bit yourself — either
gives silent no-ACKs with no obvious cause. And note that R828D **reads come back
bit-reversed** (see the gotchas below); the Si5351 reads normally.

## Implementing your own driver

`rx888_vhf.py` is the reference implementation used to validate tuner
init/standby, register writes, PLL-lock behavior, and tracking-filter
configuration on real RX888 mk2 hardware. Diff your implementation against it and
use it to track down differences.

### Bring-up sequence

1. **Reference clock** — `I2CW` Si5351 (`0xC0`): CLK2/PLL-B = 16 MHz (CLKB).
2. **Probe (verify)** — `I2CR` R828D (`0x74`) reg `0x00` → bit-reverse → expect `0x96`.
3. **Init tuner** — write the init array (`0x05`–`0x1F`), run the IF-filter cal.
4. **Tune** — `set_mux` + `set_pll`, `LO = RF + 4.57 MHz`; lock = reg `0x02` bit 6 (bit-reversed).
5. **GPIO → VHF** \* — `GPIOFX3`: set `VHF_EN` (bit 15), clear `BIAS_HF`, starting from the live GPIO word.

Teardown: R828D standby → CLKB off → GPIO back to HF.

> \* The demo driver (`vhf_tune.py` / `vhf_fm_radio.py`) actually sets the VHF
> GPIO **first**, before the clock and tuner. `VHF_EN` is the antenna-path switch
> and rides an independent bus from the I2C tuner config, so the order does **not**
> significantly change demo performance — doing it last (as shown) just avoids a
> brief transient of unlocked VHF output reaching the ADC while the tuner settles.

### Gotchas (read these first — they're what trips people up)

The full set is in the `vhf_tune.py` docstring; the load-bearing ones:

- R828D **reads are bit-reversed** (LSB-first) vs writes (MSB-first) — recover
  logical order on every read.
- Masked writes need a **software register shadow** (read-modify-write against
  the last value you wrote, not a chip read-back).
- Reset **PLL-B only** on the Si5351 — a global / PLL-A reset glitches CLKA (the
  ADC clock) and kills the stream.
- PLL lock = logical reg `0x02` bit 6 (on a bit-reversed read); raise VCO current
  on no-lock.

## Reference material

The R828D shares the R820T2's register map and tuning core; the R820T2
documentation and reference code are the closest available, and in practice work
just as well for the R828D as for the R820T. See the links at the top of
`tuner_r82xx_explained.md`.

**On accuracy — read this before treating anything here as gospel.** The register
values, field definitions, and bit names in these docs are **not guaranteed
correct**. Rafael Micro never published a full datasheet, so they are
reconstructed from the reference drivers and the R820T2 register-description PDF,
then validated empirically on hardware. They are **best-effort but sufficient**:
they largely derive from other known-working implementations and they produce a
working demo on a real RX888 mk2. Treat them as a high-quality starting point to
validate your own implementation against — not as an authoritative datasheet.
