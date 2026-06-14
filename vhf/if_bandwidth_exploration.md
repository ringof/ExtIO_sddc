# R828D IF bandwidth exploration — what we tried, what we learned

The RX888 mk2 VHF path downconverts RF to a ~4.57 MHz IF via the R828D
tuner, then the ADC samples the IF directly.  The R828D's on-chip active-RC
channel filter sets the usable IF bandwidth — nominally 6/7/8 MHz presets,
but the actual passband depends on calibration, extension bits, and
reference-clock frequency.  This doc records the exploration we did to
understand and widen it.

## The filter

The R828D channel filter is an active-RC (continuous-time) lowpass between
the mixer and the IF VGA.  Its corner frequency is set by two mechanisms:

1. **BW preset** (`set_bandwidth`): writes coarse filter-select bits to
   registers 0x0A/0x0B/0x1E.  Available presets: 8, 7, 6, 1.5 MHz.
2. **Calibration trim** (`calibrate_filter`): at init, the driver parks the
   PLL at a reference frequency, fires a one-shot calibration clock, and
   reads back FILT_CODE (reg 0x0A[3:0]).  This 4-bit code trims the RC
   corner to compensate for process variation and reference frequency.

The filter is *not* a sharp brick-wall — it has a gradual rolloff, and the
actual -3 dB point depends on the cal trim.

## Controls added to the TUI (`vhf_fm_radio.py`)

All of these are live-toggleable while receiving:

| Key | Control | Register | What it does |
|-----|---------|----------|--------------|
| `b/B` | BW preset cycle | 0x0A/0x0B/0x1E | Switch between 8/7/6/1.5 MHz presets |
| `f` | PWD_FILT | 0x0A[7] | Toggle channel filter power. OFF = bypass, dumps raw mixer IF at ADC (no selectivity, full mixer bandwidth) |
| `e` | FILTER_EXT | 0x1E[6] | Extends filter skirt. Already set by 8/7 MHz presets; toggling independently overrides the preset |
| `w` | FLT_EXT_WIDEST | 0x0F[7] | Widest filter extension. Never touched by the normal driver — init leaves it 0 |
| `i/I` | IF offset | (shifts LO) | Nudge IF ±100 kHz from nominal 4.57 MHz. Moves the LO while the filter shape stays fixed — signals slide across the passband, revealing where the filter cuts off |
| `0` | Reset IF offset | | Return to nominal 4.57 MHz |
| `r` | Ref clock toggle | Si5351 CLKB | Switch between 16 and 32 MHz reference. Affects cal result because the cal clock derives from the reference. Re-runs calibration on toggle |
| `p/P` | Cal park ±10 MHz | PLL park freq | Sweep the PLL frequency used during calibration (70.1–110.1 MHz). The cal clock comes from the PLL, so different park freqs produce different FILT_CODE values, shifting the filter corner |

## What we learned

### PWD_FILT bypass (`f`)

Powering down the channel filter (PWD_FILT=0) dumps the raw mixer output
at the ADC.  You lose all IF selectivity — every signal in the mixer's
passband hits the ADC.  Useful for confirming the filter is the bandwidth
bottleneck (it is), but not operationally useful.

### FILTER_EXT and FLT_EXT_WIDEST (`e`, `w`)

FILTER_EXT (0x1E[6]) is already exercised by `set_bandwidth` — it's on for
8 and 7 MHz presets, off for narrower.  Toggling it independently with `e`
overrides whatever the preset set.

FLT_EXT_WIDEST (0x0F[7]) is genuinely untouched by the driver — neither
librtlsdr nor ka9q-radio's r82xx code writes it.  Init value 0x68 leaves
bit 7 = 0.  Toggling it on with `w` may widen the skirt further.  Effect
is subtle and hard to measure without instrumentation.

### IF offset probing (`i/I`)

This is the most informative tool.  The IF offset shifts the LO by ±100 kHz
steps while the filter stays fixed.  As you push the offset positive or
negative, the received signal slides toward the filter edge.  When it drops
out, you've found the passband boundary.

This directly maps the filter shape without needing a spectrum analyser.

### Reference clock and calibration (`r`, `p/P`)

The cal clock derives from the PLL, which locks to the Si5351 CLKB
reference.  Changing the reference or the PLL park frequency changes the
cal clock, which changes the FILT_CODE trim, which shifts the filter corner.

**The 56 MHz cal park trap:** the original default park was 56 MHz.  This
never locked — two compounding problems:

1. **VCO floor:** 56 × 32 = 1792 MHz, barely above the ~1770 MHz VCO
   minimum.  Marginal at best.
2. **Integer-N:** at both 16 and 32 MHz refs, 56 MHz produces SDM = 0
   (exact integer division) with dither off.  Integer-N PLLs are harder to
   lock than fractional-N.

Fix: changed default park to **100.1 MHz** — mid-VCO range, non-integer at
both 16 and 32 MHz refs (SDM ≠ 0).  Locks reliably.

**Missing `_set_mux` in cal path:** the ported `calibrate_filter` initially
called `_set_pll` without `_set_mux`, leaving reg 0x10 at the init value
(10 pF cap loading the externally-driven CLKB pin via the crystal circuit).
Adding `_set_mux(park_lo)` before `_set_pll` fixed it — `_set_mux` sets
the reference divider and clears the cap bits.

### Reference clock max

R828D PLL constraint: `nint` (integer divider) must be ≥ 13.  With
VCO_MIN ~1770 MHz: max ref = VCO_MIN / (2 × 13) ≈ 68 MHz.  For FM
broadcast (88–108 MHz), the actual max is slightly higher (~69 MHz) because
the LO is higher.  The 16/32 MHz toggle is well within limits.

## ka9q-radio register comparison

Compared our init/standby arrays (librtlsdr-derived) against ka9q-radio's
`rx888.c` / `r820t.h`.  Found 3 init differences and 4 standby differences.
All are operating-point choices (charge pump current, power-detect gain,
clock output enable), not bugs — except one:

**`r820t.h` bug:** `#define R820T_R9_PWD_IFFILT (1<7)` — comparison (always
1), not bit-shift (`1<<7` = 0x80).  Result: ka9q's standby writes 0x41 to
reg 0x09 instead of 0xC0, leaving the IF filter partially powered during
standby.  We kept our 0xC0.

The ka9q init values were swapped in and confirmed working with FM reception.
