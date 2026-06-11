# How the R820T2 / R828D Tuner Works

A walk-through of the Rafael Micro **R820T2 / R828D** silicon tuner as actually
driven by `SDDC_FX3/driver/tuner_r82xx.c` (recoverable from commit `0ffa512`).
Every register and constant below is cited to a line in that file so it can be
checked against the source.

The chip is a **superheterodyne front-end on a die**: it slides any RF signal
down to one fixed intermediate frequency (IF) that the ADC then samples.
Everything is configured over I2C through ~32 registers (`0x00`–`0x1f`);
`0x00`–`0x04` are read-only status (PLL lock, VCO fine-tune), the rest control.

---

## 1. Signal path

```mermaid
flowchart LR
    ANT([Antenna]) --> LNA["LNA<br/>(low-noise amp)"]
    LNA --> TF["Tracking bandpass<br/>(preselector)<br/>regs 0x1a/0x1b"]
    TF --> MIX{{"Mixer"}}
    MIX --> VGA["IF VGA<br/>(gain stages)"]
    VGA --> IFFILT["IF channel filter<br/>regs 0x0a/0x0b/0x1e"]
    IFFILT --> IFOUT["IF out (3.57 / 4.57 MHz)"]
    IFOUT --> ADC[(LTC2208 ADC)]

    LO["Local oscillator"] --> MIX
    PLL["Fractional-N PLL<br/>VCO 1770-3540 MHz"] --> LO
    REF["Reference clock<br/>(Si5351 CLKB in RX888)"] --> PLL

    classDef ext fill:#f9f3e0,stroke:#b8860b;
    class REF,ADC ext;
```

The two highlighted blocks are **off the tuner die**: the reference clock comes
from the Si5351 (CLKB), and the IF lands in the RX888's ADC. Everything between
is on-chip and set over I2C.

Key fact: the mixer produces `|RF − LO| = IF`, so the ADC always sees a **fixed
IF** regardless of what you tune. The IF value is whatever the channel filter
selects (§5): the chip default is `R82XX_IF_FREQ = 3.57 MHz`, but **this
firmware's 8 MHz filter setting puts it at 4.57 MHz**.

---

## 2. The superheterodyne relationship

```mermaid
flowchart LR
    subgraph spectrum["Frequency axis"]
        direction LR
        RF["RF signal<br/>(e.g. 100.00 MHz)"]
        IFc["IF<br/>3.57 MHz"]
        LOc["LO<br/>103.57 MHz"]
        IMG["IMAGE<br/>107.14 MHz<br/>(LO + IF)"]
    end
    RF -->|"mixer: RF − LO"| IFc
    LOc -.->|"sets the offset"| RF
    IMG -.->|"rejected by<br/>tracking filter"| TFnote["§4"]
```

You never tell the chip the RF directly — you place the **LO**, and the mixer
does the subtraction. From `r82xx_set_freq64` (line 1992):

```c
lo_freq = freq + priv->int_freq + priv->if_band_center_freq;  // or − for other sideband
```

- `priv->int_freq` = the IF (3.57 MHz default; **4.57 MHz** with this
  firmware's 8 MHz filter — §5). The worked numbers above use the 3.57 default.
- `priv->sideband` chooses high-side vs low-side LO injection (flips the spectrum).
- The **image** at `LO + IF` would also mix down to the IF — that is what the
  tracking filter (§4) exists to suppress.

---

## 3. Tuning control flow

What happens on one `r82xx_set_freq64()` call (line 1992):

```mermaid
flowchart TD
    A["Desired RF freq"] --> B["Compute LO<br/>LO = RF ± IF ± if_center<br/>(line 2013-2016)"]
    B --> C["r82xx_set_mux(LO)<br/>tracking filter + RF mux<br/>(line 730)"]
    C --> D["r82xx_set_pll(LO)<br/>program fractional-N PLL<br/>(line 931)"]
    D --> E{"PLL locked?<br/>reg 0x00, data[2] & 0x40"}
    E -->|yes| F["R828D: pick Air-In vs Cable1<br/>switch at 345 MHz, reg 0x05<br/>(line 2055)"]
    F --> G([Tuned])
    E -->|"no, and RF high enough"| H["Retry on 5th harmonic<br/>(line 2034)"]
    H --> C
    E -->|"no"| X([Return PLL-not-locked error])
```

The **harmonic retry** is how the chip reaches above its native VCO/mixer
ceiling: above a threshold it tunes the LO to `LO/5` and uses the 5th harmonic
of the mixer (`lo_freqHarm = lo_freq / harm`, line 2022).

---

## 4. The tracking filter — why `freq_ranges[]` exists

A mixer also responds to the **image** (`LO + IF`) and to strong out-of-band
signals, so the chip carries an on-chip bandpass that must **move with the
tuning**. `r82xx_set_mux` (line 730) looks the LO up in `freq_ranges[]`
(line 370) and programs the band:

```mermaid
flowchart TD
    LO["LO frequency"] --> LOOKUP["Find band in freq_ranges[]<br/>(line 738-742)"]
    LOOKUP --> R17["reg 0x17 — open drain"]
    LOOKUP --> R1A["reg 0x1a — RF mux / polyphase"]
    LOOKUP --> R1B["reg 0x1b — TF band<br/>(tracking-filter cap bank)"]
    LOOKUP --> R10["reg 0x10 — crystal cap / drive"]
```

Each `freq_ranges[]` entry is `{ start_freq, open_d, rf_mux_ploy, tf_c,
xtal_caps... }`. As you tune, a different capacitor bank (`tf_c`) is switched in
so the RF bandpass stays centered on the wanted signal.

> This is exactly why "tune by moving the reference clock only" (Model 2 in the
> VHF plan) degrades image rejection: it shifts the LO **without** re-running
> `set_mux`, so the tracking filter stays parked on the wrong band.

### Tracking-filter band table (`freq_ranges[]`, line 370)

The LO frequency selects a row; `tf_c` is the tracking-filter capacitor bank
(reg `0x1b`) and shrinks as the LO climbs (smaller cap → higher resonance).
Above ~310 MHz the RF mux switches from the LPF to **bypass**.

| LO band ≥ (MHz) | `tf_c` (R`0x1b`) | RF mux (R`0x1a`[7:6]) | poly (R`0x1a`[1:0]) | open-drain (R`0x17`) |
|---:|:---:|:---:|:---:|:---:|
| 0   | `0xDF` | LPF | low | low |
| 50  | `0xBE` | LPF | low | low |
| 55  | `0x8B` | LPF | low | low |
| 60  | `0x7B` | LPF | low | low |
| 65  | `0x69` | LPF | low | low |
| 70  | `0x58` | LPF | low | low |
| 75  | `0x44` | LPF | low | high |
| 80  | `0x44` | LPF | low | high |
| 90  | `0x34` | LPF | low | high |
| 100 | `0x34` | LPF | low | high |
| 110 | `0x24` | LPF | low | high |
| 120 | `0x24` | LPF | low | high |
| 140 | `0x14` | LPF | low | high |
| 180 | `0x13` | LPF | low | high |
| 220 | `0x13` | LPF | low | high |
| 250 | `0x11` | LPF | low | high |
| 280 | `0x00` | LPF | low | high |
| 310 | `0x00` | **bypass** | middle | high |
| 450 | `0x00` | **bypass** | middle | high |
| 588 | `0x00` | **bypass** | highest | high |
| 650 | `0x00` | **bypass** | highest | high |

---

## 5. The IF channel filter — bandwidth options (`r82xx_set_bandwidth`, line 1759)

After the mixer, the chip has a **configurable IF channel filter** that sets
how wide a slice survives around the IF center. Two things to know:

1. **The IF center is *not* fixed** at the chip's `R82XX_IF_FREQ` (3.57 MHz)
   default — `set_bandwidth` overwrites `priv->int_freq` from the chosen filter
   row. **This firmware calls it with 8 MHz**
   (`r82xx_set_bandwidth(&tuner, 8*1000*1000, …)` in the old USBhandler), which
   lands the IF center at **4.57 MHz**, not 3.57.
2. Wide bandwidths (≥6 MHz) are hardcoded; narrower ones come from the `IFi[]`
   table (line 1669), and there the IF center = `fif + fc` and shifts per row.

### Wide presets (hardcoded, top of `set_bandwidth`)

| Requested | Applied BW | R`0x0a` | R`0x0b` | **IF center** |
|---:|:---:|:---:|:---:|:---:|
| > 7 MHz | 8 MHz | `0x10` | `0x0b` | **4.57 MHz** ← used by this firmware |
| > 6 MHz | 7 MHz | `0x10` | `0x2a` | 4.57 MHz |
| > 4.5 MHz | 6 MHz | `0x10` | `0x6b` | 3.57 MHz |

### Narrow steps (`IFi[]`, "centered" rows; IF center = `fif + fc`)

| 3-dB BW (kHz) | IF center (kHz) | R`0x0a` | R`0x0b` | R`0x1e` |
|---:|---:|:---:|:---:|:---:|
| 290  | 1925 | `0x0F` | `0xE7` | `0x00` |
| 375  | 1857 | `0x0F` | `0xE8` | `0x00` |
| 420  | 2121 | `0x0F` | `0xD7` | `0x00` |
| 470  | 1788 | `0x0F` | `0xE9` | `0x00` |
| 600  | 1706 | `0x0F` | `0xEA` | `0x00` |
| 860  | 1558 | `0x0F` | `0xEB` | `0x00` |
| 950  | 2205 | `0x0F` | `0x88` | `0x00` |
| 1100 | 2125 | `0x0F` | `0x89` | `0x00` |
| 1200 | 1350 | `0x0F` | `0xEE` | `0x00` |
| 1300 | 2043 | `0x0F` | `0x8A` | `0x00` |
| 1500 | 1276 | `0x0F` | `0xEF` | `0x60` |
| 1600 | 1900 | `0x0F` | `0x8B` | `0x00` |
| 1750 | 1412 | `0x0F` | `0xCF` | `0x60` |
| 1800 | 1400 | `0x0F` | `0xAF` | `0x00` |
| 1950 | 1530 | `0x0F` | `0x8F` | `0x60` |
| 2200 | 1600 | `0x0F` | `0x8F` | `0x00` |
| 3000 | 2000 | `0x04` | `0x8F` | `0x00` |
| 5000 | 3570 | `0x0B` | `0x6B` | `0x00` |

> `R0x0a` low nibble + `R0x0b` set the filter corner/Q; `R0x1e` (`0x60`) enables
> the "filter extension on weak signal." When `WITH_ASYM_FILTER` is compiled in,
> each row also gets steep-low / steep-high **asymmetric** variants
> (`sharpCorner` 1/2, via the `IFA()`/`IFB()` macros) for single-sideband use —
> those are the `r82xx_set_sideband` (reg `0x07` bit 7) companions.

```mermaid
flowchart LR
    REQ["requested BW"] --> SEL{"≥ 6 MHz?"}
    SEL -->|yes| WIDE["hardcoded preset<br/>R0x0a/R0x0b<br/>IF center 3.57 / 4.57 MHz"]
    SEL -->|no| TBL["scan IFi[] by median BW<br/>(line 1789)"]
    TBL --> NARROW["row → R0x0a/R0x0b/R0x1e<br/>IF center = fif + fc"]
    WIDE --> IF["set priv->int_freq"]
    NARROW --> IF
```

---

## 6. Fractional-N PLL — how the LO is synthesized

The core of `r82xx_set_pll` (line 931). The VCO only runs **1770–3540 MHz**
(`vco_min`, `vco_max`), so the LO is the VCO divided down:

```mermaid
flowchart TD
    LO["Target LO (kHz)"] --> DIV["Pick mix_div ∈ {2,4,8,...,64}<br/>so vco_min ≤ LO×mix_div < vco_max<br/>(line 1006 while-loop)"]
    DIV --> VCO["vco_freq = LO × mix_div<br/>(line 1042)"]
    VCO --> FRAC["vco_div = (ref + 65536·vco_freq) / (2·ref)<br/>nint = vco_div / 65536<br/>sdm  = vco_div mod 65536<br/>(line 1063-1065)"]
    FRAC --> WR["Write registers:<br/>0x14 = ni + (si<<6)  ← integer<br/>0x16 = sdm >> 8       ← frac hi<br/>0x15 = sdm & 0xff     ← frac lo"]
    WR --> SLEEP["sleep DELAYPLL<br/>(line 1124)"]
    SLEEP --> LOCK{"reg 0x00<br/>data[2] & 0x40<br/>locked?"}
    LOCK -->|yes| DONE["set autotune 8 kHz<br/>has_lock = 1"]
    LOCK -->|no| BUMP["Raise VCO current<br/>reg 0x12 bits[7:5]<br/>retry once"]
    BUMP --> LOCK
```

The synthesized LO is therefore:

```
            2 · ref · (nint + sdm/65536)
   LO  =  ───────────────────────────────
                     mix_div
```

where `ref = priv->cfg->xtal` — **the external reference clock**. In the RX888
that reference is the **Si5351 CLKB output**. So:

- The integer `nint` (8-ish bits) sets the coarse step; the 16-bit `sdm`
  sigma-delta value fills in the fraction → very fine resolution.
- **Every bit of LO accuracy rides on `ref`.** Control the Si5351 precisely and
  the whole equation is deterministic — which is the entire reason for the
  "know the exact frequency via the clock" discussion.
- If the VCO won't lock, the driver **raises VCO bias current** (reg `0x12`)
  and retries — an autonomous calibration, not a host concern.

---

## 7. Gain stages

Three independent gain blocks, each its own I2C control (mapped to the old
firmware's `SETARGFX3` sub-commands):

```mermaid
flowchart LR
    IN["RF in"] --> LNAg["LNA gain<br/>(R82XX_ATTENUATOR)"]
    LNAg --> MIXg["Mixer gain<br/>(R82XX_ATTENUATOR)"]
    MIXg --> VGAg["IF VGA gain<br/>(R82XX_VGA)"]
    VGAg --> OUT["IF out"]
```

Each can run in auto (AGC) or manual mode; `set_all_gains` / `set_vga_gain`
walk gain-index tables to register values.

---

## 8. Boot / init

`r82xx_init` (line 2132) writes `r82xx_init_array[]` (line 330) — ~27 bytes of
sane defaults for regs `0x05`–`0x1f` — then runs `set_bandwidth` to calibrate
the IF channel filter. After that the chip is idle until the first
`set_freq64`.

```mermaid
flowchart LR
    P["power / reference up"] --> INIT["r82xx_init:<br/>blast init_array regs 0x05-0x1f"]
    INIT --> BW["set_bandwidth:<br/>IF filter calibration"]
    BW --> IDLE([idle, awaiting set_freq])
```

---

## Register cheat-sheet (the load-bearing ones)

| Reg | Role | Set by |
|-----|------|--------|
| `0x00`–`0x04` | **Read-only status** — PLL lock (`data[2] & 0x40`), VCO fine-tune | read in `set_pll` |
| `0x05` | LNA / loop-through; R828D **Air-In vs Cable1** input (`0x60`) | `init`, `set_freq64` |
| `0x07` | **Sideband** select (bit 7) | `set_sideband` |
| `0x0a`/`0x0b` | **IF channel-filter** bandwidth / corner | `set_bandwidth` |
| `0x0e` | mixer AGC threshold | `set_bandwidth` |
| `0x1e` | IF filter extension "on weak signal" (`0x60`) | `set_bandwidth` |
| `0x10` | Reference divider, crystal cap, PLL divider (`div_num`) | `set_mux`, `set_pll` |
| `0x12` | **VCO current** (bits 7:5), dither/SDM power | `set_pll` |
| `0x14` | PLL integer `nint` → `ni + (si<<6)` | `set_pll` |
| `0x15`/`0x16` | PLL fractional `sdm` (lo / hi byte) | `set_pll` |
| `0x1a` | PLL autotune bw; **RF mux / polyphase** | `set_pll`, `set_mux` |
| `0x1b` | **Tracking-filter band** (`tf_c`) | `set_mux` |
| `0x17` | Open-drain | `set_mux` |

---

### One-line summary

> A moving RF bandpass + a divided-down fractional-N LO + a fixed mixer, all
> poked through ~30 I2C registers, dropping everything to a 3.57 MHz IF for the
> ADC — and the only knob that *isn't* on the chip is the reference clock
> (Si5351 CLKB).
