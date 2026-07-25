# Plan — Low/High-Side Injection Selection in the VHF TUI

Status: **DRAFT — awaiting approval.** No code changed yet.

## Goal

Let the operator switch the R828D between **high-side** (`LO = RF + IF`, the
current fixed behavior) and **low-side** (`LO = RF − IF`) injection from the
`vhf_fm_radio.py` TUI, reprogramming the tuner correctly for the chosen side
and surfacing the resulting spectral-inversion state so the downstream host
(ka9q-radio) can be set up to match.

## Why

- Today the driver is hard-wired high-side (`rx888_vhf.py:316`,
  `lo = rf_hz + self.if_hz`) with the mixer sideband bit statically 0
  (`0x07 = 0x70` in the init array, `rx888_vhf.py:85`). High-side injection
  **inverts** the spectrum, which is exactly the stereo/RDS-won't-lock caveat
  in `vhf_fm_howto.md:148`.
- Low-side injection produces a **non-inverted** spectrum (fixes that caveat)
  but moves the image to `RF − 2·IF` (~9.14 MHz below the wanted signal),
  changing tracking-filter image rejection. So a toggle is a genuine
  inversion-vs-image-rejection operator lever, not just plumbing.

## Correctness requirement

The LO formula and the mixer sideband bit **must stay in lockstep**. Per
`tuner_r82xx_explained.md:94–95, 234, 429`: `sideband == 0` ⇒ LO above RF
(high-side); `0x07` bit 7 is the sideband select. The safest design is to
recompute the LO **and** write the sideband bit together in one place, every
tune, so they can never desync.

## Changes

### 1. Driver — `vhf/rx888_vhf.py`

- Add `self.sideband_low = False` state (default `False` = high-side =
  current behavior; zero behavior change until toggled).
- Add `set_sideband(low)`:
  ```python
  def set_sideband(self, low):
      # 0x07[7]: 0 = high-side (LO above RF), 1 = low-side. Polarity per
      # r82xx lineage (explained.md:94); BENCH-CONFIRM on the RX888 mk2.
      self._wr_mask(0x07, 0x80 if low else 0x00, 0x80)
  ```
  Neither `set_mixer_gain` (mask `0x0F`) nor `set_mixer_agc` (mask `0x10`)
  touches bit 7, so the setting persists across gain/AGC changes.
- In `r828d_set_freq` (line 316), branch the LO and assert the bit in lockstep:
  ```python
  lo = rf_hz - self.if_hz if self.sideband_low else rf_hz + self.if_hz
  self.set_sideband(self.sideband_low)   # keep 0x07[7] matched to the LO
  ```
- Add `inverted` helper: high-side ⇒ inverted; low-side ⇒ non-inverted.

### 2. TUI — `vhf/vhf_fm_radio.py`

- New state `self._sideband_low` (default `False`).
- New key **`s`** (unused in `on_key`, verified) → `_toggle_sideband()`,
  cloned from `_toggle_ref` (line 407): set state → `set_sideband` →
  `calibrate_filter(cal_park)` → `r828d_set_freq(freq)` → status message.
- Display: extend the Bandwidth/IF line (line 231) with the active side and
  inversion, e.g. `LO− (spectrum: normal)` / `LO+ (spectrum: INVERTED)`,
  color-coded (INVERTED in yellow, as a caution).
- Help text (`HELP_TEXT`, line 60): add `s  toggle LO side (high/low)`.

### 3. Host inversion signal (the "+ host inversion signal" scope)

Honest constraint: the TUI controls the tuner over EP0, while ka9q-radio
streams the ADC in a **separate process** — there is no in-band metadata
channel between them, so inversion can't be auto-negotiated. What we *can* do:

- Make inversion **visible** in the TUI (item 2 above) so the operator knows
  the current state at a glance.
- Document the downstream action in `vhf/vhf_fm_howto.md`: how to tell
  ka9q-radio the spectrum is inverted (it has per-channel spectral-inversion
  handling), and that low-side removes the need for it. Update the line-148
  caveat to point at the new toggle.

### 4. Optional refinement (flag for approve/trim)

Fuller image-rejection fidelity would also flip the IF filter's asymmetric
"sharp-corner" companion bits (the `IFA()/IFB()` pairing, `explained.md:233–
234`) per side. These live in `0x0A`/`0x0B`, which `set_bandwidth` already
writes (mask `0x0F` / `0xEF`, line 496–497), so it needs careful interaction
with the bandwidth presets. **Proposed: defer** unless bench testing shows the
minimal change leaves image rejection unacceptable on one side.

## Validation test (bench)

1. Launch TUI, tune a strong local FM station, confirm `LOCKED`, note audio.
2. Press `s` → confirm log shows a fresh `LO=… lock=YES` with LO now
   `RF − IF` (≈9.14 MHz lower than the high-side value), and the display flips
   to the low-side / non-inverted state.
3. In ka9q-radio, confirm the station now decodes **without** the host
   inversion setting (and that stereo/RDS behavior tracks the side).
4. Toggle back with `s`; confirm return to high-side LO and inverted state.
5. Read back `0x07` bit 7 via I2C and confirm it matches the displayed side.

## Regression test

- With no `s` press, every value/register write is byte-identical to today
  (default `sideband_low=False`, LO `RF+IF`, `0x07[7]=0`) — verify by diffing
  the init + first-tune register trace against a pre-change run.
- Existing keys (`r`, `b/B`, gains, filters, IF offset, cal park) behave
  unchanged; `s` interacts cleanly with a subsequent `r`/`b` (side persists
  across ref and bandwidth changes because `0x07[7]` isn't touched by those
  paths).
- `q` standby still powers down cleanly.

## Files touched

- `vhf/rx888_vhf.py` — `set_sideband`, LO branch, state, `inverted` helper.
- `vhf/vhf_fm_radio.py` — state, `s` handler, display, help text.
- `vhf/vhf_fm_howto.md` — inversion/host note + line-148 caveat update.
- (this plan doc)
