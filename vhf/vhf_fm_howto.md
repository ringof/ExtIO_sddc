# HOWTO — VHF FM broadcast reception on the RX888 mk2 with ka9q-radio

A worked, end-to-end procedure for testing the RX888's VHF front end (R828D
tuner) by listening to the FM broadcast band through ka9q-radio. radiod streams
the ADC; a small host-side script tunes the tuner. They run **concurrently**.

## The mental model (read this first)

The RX888's Si5351 has two independent clock outputs, and that separation is the
whole trick:

```
            Si5351
   CLKA (CLK0/PLL-A) ── ADC sample clock ──────────►  radiod  (owns this)
   CLKB (CLK2/PLL-B) ── R828D reference ───────────►  vhf_tune.py (owns this)

   FM station @ RF ──[R828D: LO = RF + 4.57 MHz]──► IF 4.570 MHz ──► ADC ──► radiod
```

- **radiod** drives the RX888 over its normal direct-sampling path and owns the
  **ADC sample clock (CLKA)**. It has no idea the R828D exists — it just sees the
  ADC baseband spectrum.
- **`vhf_tune.py --persist`** tunes only the **VHF front end** over EP0 (flips
  `VHF_EN`, programs **CLKB** = 16 MHz tuner reference, sets the R828D PLL). It
  never touches CLKA, so it runs alongside radiod (the two-actor EP0/EP1 model).
- Because the tuner uses `LO = RF + 4.57 MHz`, the station you tune to always
  lands at **4.570 MHz** in the ADC spectrum. The WBFM receiver in
  `rx888-vhf-fm.conf` is parked exactly there.

## Prerequisites

- The `ka9q-radio` Docker image built (`docker build -t ka9q-radio docker/ka9q-radio/`).
- `SDDC_FX3/SDDC_FX3.img` present (radiod uploads it if the device is in the
  bootloader).
- `pyusb` on the host (`pip install pyusb`) for the tuner script.
- Firmware that exposes GPIO state in GETSTATS (so the tuner reads the live GPIO
  word). Older firmware (release 0.1.0) needs `--base` — see Troubleshooting.

## Procedure

### 1. Start radiod with the VHF/FM config

```
./docker/ka9q-radio/ka9q.sh start --vhf
```

This runs radiod with `rx888-vhf-fm.conf` (a WBFM receiver at 4.57 MHz),
bind-mounted so no image rebuild is needed. Watch the logs once:

```
docker logs -f ka9q-radio
```

First launch regenerates FFTW wisdom for the 384 kHz WBFM channel (cached in the
`wisdom/` volume afterward, so subsequent starts are fast). Wait for radiod to
report the stream is established before tuning.

### 2. Tune the front end to a station

Pick a strong local FM station and give its frequency **in Hz**:

```
python3 vhf/vhf_tune.py 100300000 --persist   # 100.3 MHz
```

Expect the tuner's checkpoints: `reg0=0x96 ... OK`, `CLKB ... enabled`, and
`lock=YES`. It leaves the tune active (`--persist`) and exits.

### 3. Listen

```
./docker/ka9q-radio/ka9q.sh monitor fm-pcm.local
```

(Needs `/dev/snd` on the host for audio — see the ka9q.sh start notes.)

### 4. Hop stations — two ways

- **Fine (within the captured window):** the R828D's 8 MHz IF filter also passes
  neighbours (~4.57 MHz ± 4 MHz). Retune the *receiver* without touching the
  front end:
  ```
  ./docker/ka9q-radio/ka9q.sh console
  control hf.local                  # curses tuner; move the FM-BCB channel
  ```
- **Coarse (move the whole window):** re-run the tuner for a station outside the
  current window:
  ```
  python3 vhf/vhf_tune.py 88500000 --persist
  ```

## Back to HF / cleanup

```
python3 vhf/vhf_tune.py --standby    # R828D off, CLKB off, GPIO back to HF
./docker/ka9q-radio/ka9q.sh stop     # stop radiod
```

(`--standby` flips the front end back to HF while radiod is still up, which is
fine; or just stop the container.)

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Tuner: `GETSTATS does not expose GPIO state` | Older firmware (≤ 0.1.0). Pass an explicit HF GPIO word: `python3 vhf/vhf_tune.py 100300000 --persist --base 0x00000`. |
| `reg0 != 0x96` / `ID MISMATCH` | R828D not reachable. Don't `--force` past it — check the device is in app mode and radiod is actually running (front-end power/clocks). |
| `lock=NO` | LO out of range or wrong reference. Confirm the frequency is real FM-band Hz; try `--ref 16000000` explicitly. |
| Audio silent in `monitor` | Host has no `/dev/snd`, or `fm-pcm.local` not resolving. Check `docker logs ka9q-radio` for "Established under name". |
| It was working, then went dead after I toggled dither/rand in ka9q | radiod rewrote the whole GPIO word and cleared `VHF_EN`. Re-run `python3 vhf/vhf_tune.py <freq> --persist`. Don't toggle dither/rand during VHF use. |
| First `--vhf` start is slow | One-time FFTW wisdom generation for the 384 kHz channel. Cached after. Set `FFTW_RIGOR=estimate` for an instant (slower-runtime) build. |
| Stereo/RDS won't decode | High-side LO (`LO = RF + IF`) inverts the spectrum. Mono FM audio is unaffected; stereo subcarriers may not lock. |

## Reference

| Item | Value |
|---|---|
| Config | `docker/ka9q-radio/rx888-vhf-fm.conf` |
| Launcher | `./docker/ka9q-radio/ka9q.sh start --vhf` |
| Tuner | `python3 vhf/vhf_tune.py <fm_hz> --persist` |
| Receiver park frequency | 4.570 MHz (`IF_CARRIER`); `LO = RF + 4.57 MHz` |
| Tuner reference (CLKB) | 16 MHz |
| ADC sample rate | 64.8 MHz (`samprate = 64m8`) |
| Stream name | `fm-pcm.local` |
| Status group | `hf.local` (`control hf.local`) |

See also: `tuner_r82xx_explained.md` (R828D chip internals),
`rx888_vhf.py` (shared driver module), `vhf_tune.py` (CLI reference),
`vhf_fm_radio.py` (interactive TUI), `docker/ka9q-radio/README.md` (the container).

---

## Appendix: driver-author wire-format reference

Everything below is for writing a C/C++ host driver. The Python tools
(`rx888_vhf.py`) already implement all of it.

### EP0 commands used

| Need | Command | Notes |
|---|---|---|
| HF/VHF switch | `GPIOFX3` (`0xAD`) | 32-bit LE word in data phase (`wLength=4`) |
| Tuner reference clock | `I2CWFX3` (`0xAE`) to Si5351 (`0xC0`) | Program CLK2/PLL-B = 16 MHz |
| R828D tune / gain / BW | `I2CWFX3` / `I2CRFX3` (`0xAF`) to `0x74` | `wValue`=addr, `wIndex`=reg, `wLength`=count |
| Firmware liveness | `TESTFX3` (`0xAC`) | Returns `[hwconfig, fw_hi, fw_lo, rqt_count]` |

### What to do (the four steps)

1. **Select VHF** — `GPIOFX3` with `VHF_EN` (bit 15) set, `BIAS_HF` cleared.
   It's a whole-word write, so start from your existing HF GPIO word and flip
   the bits — everything else (ADC settings, etc.) carries over.

2. **Tuner reference** — program Si5351 **CLK2 (CLKB)** to **16 MHz**, exactly
   as you program CLK0 for the ADC. Disable it in HF mode to keep spurs out.

3. **Tune the R828D** — init the register array (regs 0x05–0x1f), then
   `set_mux` + `set_pll` with `LO = RF + IF`. Probe first: read reg 0x00,
   bit-reverse the byte, expect `0x96`.

4. **Stream and look** — the station lands at **IF = 4.57 MHz** in the ADC
   spectrum. That's the proof.

### Read-back checks (don't fly blind)

- **Firmware alive:** `TESTFX3` → `hwconfig = 0x04`
- **CLKB on:** Si5351 CLK2_CONTROL (reg 18), bit 7 clear = enabled
- **Tuner reachable:** reg 0x00 bit-reversed == `0x96`
- **PLL locked:** reg 0x02 bit-reversed, `& 0x40` = lock

### Tuner code — use clean upstream

The firmware's `tuner_r82xx.c` is an FX3-mangled fork (Cypress types,
embedded sleeps) — use **upstream librtlsdr** (`steve-m/librtlsdr` →
`src/tuner_r82xx.c`) as the porting base. Board specifics: R828D at `0x74`;
reference is **Si5351 CLKB, not a fixed crystal** — set `cfg->xtal` to
whatever you program CLK2 to; and the IF is **4.57 MHz** with the 8 MHz
filter.
