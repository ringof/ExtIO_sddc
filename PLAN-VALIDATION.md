# PLAN — end-to-end firmware validation (`validate.sh`)

A single top-level wrapper that runs the three firmware-validation stages in
sequence against an attached RX888mk2, each with explicit pass/fail, ending in
one overall PASS/FAIL. Hardware-only (privileged USB), never CI.

## Stages and pass/fail

| # | Stage | Tool | PASS criteria |
|---|-------|------|---------------|
| 1 | Vendor-command + data-flow correctness | `tests/fw_test.sh` | TAP: every test `ok`, `0 failed` |
| 2 | Long-run stability | `tests/soak_test.sh` / `fx3_cmd soak` | runs the configured duration with no wedge/overrun/frozen-DMA/stall |
| 3 | Runs under ka9q-radio **and produces real output** | docker `ka9q-radio` + `ka9q_smoke.sh` + `fx3_cmd stats` | all of the Stage-3 gates below |

Stages are sequential and each **releases the USB device before the next**
(fw_test/soak claim it via `fx3_cmd`; Stage 3's radiod claims it exclusively).

## Stage 3 pass/fail — "produces real output"

Run `ka9q_smoke.sh` (full `0..fs/2` sweep through radiod's `rx888.so`), then
the stop→idle check. Hard gates:

1. **Streams** — `powers` returns a non-empty spectrum; radiod logged
   `rx888 running` + channels started, no fatal markers.
2. **Sane floor** — floor mean within a generous window (default −150…−110 dB).
   Catches all-zeros (too low) and railing/clipping (too high).
3. **Texture** — spread (max−min) ≥ `MIN_SPREAD` (default 5 dB). The
   live-vs-frozen discriminator: thermal noise has natural variance; a
   shut-down/frozen ADC FFTs to a flat line (~0 dB).
4. **fs/2 Nyquist alias present** *(per decision — RX888-specific gate)* — a
   peak within a window around `fs/2` (≈32.4 MHz for 64.8 Msps) at least
   `ALIAS_MIN_DB` (default 20 dB) above the median floor. Measured reference:
   alias ≈ −84 dB vs ≈ −132 floor → ~48 dB, so 20 dB is comfortably safe.
   This is strong, antenna-independent proof the ADC is genuinely sampling at
   `fs`. **Documented coupling caveat:** this gate assumes the firmware's
   DC-offset→Nyquist alias exists; if a future firmware nulls the DC offset
   the threshold/window must be revisited or the gate relaxed.
5. **Clean idle after stop** (#131) — after radiod stops: `fx3_cmd stats`
   shows GPIF idle, DMA count frozen across two reads, and the ADC parked in
   SHDN. (`ka9q_test.sh` already implements this assertion; reuse it.)
6. **Kill and return** — after stopping radiod, **restart it and confirm it
   comes back up streaming** (`rx888 running` again + a live spectrum on the
   restarted session). This validates the full stop→standby→wake→re-stream
   cycle (#131) through the real host, and is exactly the restart path that
   stalled on the bench (`ka9q_test.sh` cycles 2/3 reached "found rx888 /
   Si5351 programmed" but never `rx888 running`). Run a few cycles, not just
   one.

A PNG of the sweep + the measured numbers (floor mean/spread, alias level,
fs/10 birdie) are logged as corroboration for a human, but only gates 1–6
decide pass/fail.

## `validate.sh` design

Location: `tests/validate.sh`. TAP-ish per-stage output + a final summary.
Stage 3 is split so each tool does what it's best at:
- **3A — real output (rich gates 1–4):** one full `ka9q_smoke.sh` `0..fs/2`
  sweep on a fresh radiod (floor mean, spread, fs/2 alias).
- **3B — idle + kill-and-return (gates 5–6):** `ka9q_test.sh` for a few short
  start/stop cycles — each cycle asserts the device parked clean after stop
  (gate 5) and that radiod came back up streaming a live spectrum (gate 6).
  This is the harness that exposed the bench restart stall, so it doubles as
  the regression guard for it.

Options:
- `--firmware PATH` (default `SDDC_FX3/SDDC_FX3.img`)
- `--stages 1,2,3` (default all; lets you run one stage)
- `--soak-secs N` (default short, e.g. 300, for a smoke-grade run; document
  that a real validation uses hours) / `--skip-soak`
- `--container NAME`, `--image NAME`
- `--keep-going` (run remaining stages even after a failure; default: stop)

Flow:
1. **Preflight** — RX888 present (`lsusb 04b4:00f1/00f3`); `fx3_cmd` built
   (else `make -C tests`); for Stage 3, docker image present (else instruct
   `docker build`). Bail with a clear message if unmet.
2. **Stage 1** — `fw_test.sh`; parse TAP, fail iff any `not ok`.
3. **Stage 2** — `soak_test.sh`/`fx3_cmd soak` for `--soak-secs`; fail on any
   wedge/overrun/stall.
4. **Stage 3A** — start container (`ka9q.sh start`), wait for radiod
   streaming, `ka9q_smoke.sh` (gates 1–4), then stop the container to free the
   device.
5. **Stage 3B** — `ka9q_test.sh` for a few short cycles (`--duration` sized to
   ~3 cycles, `--reload-interval` ≥ duration so it's pure start/stop): each
   cycle gives gate 5 (clean idle after stop) and gate 6 (radiod returns and
   streams). Pass iff every cycle's TAP line is `ok`.
6. **Summary** — `Stage N: PASS/FAIL` lines + overall; exit 0 iff all run
   stages passed.

## Changes required

1. **NEW `tests/validate.sh`** — the wrapper above.
2. **`tests/ka9q_smoke.sh`** — add gate 4 (fs/2 alias check) with
   `ALIAS_FREQ` (default 32400000), `ALIAS_WINDOW` (default ±200 kHz),
   `ALIAS_MIN_DB` (default 20). Compute median floor excluding the DC and
   alias regions; PASS only if the alias peak clears it. Keep gates 1–3.
   Update the script header + PASS/FAIL messages.
3. **`tests/README.md`** — document `validate.sh`, the three-stage flow, and
   the Stage-3 criteria; add to the file map.
4. **`tests/ka9q_test.sh`** — reused as-is for Stage 3B (already does
   start/stop cycling + per-cycle idle and live-RF checks). NOTE: the bench
   cycles-2/3 restart stall (`RESUME-ka9q-test.md`) is still open — Stage 3B
   is the gate that will (correctly) fail on it until it's fixed, so it
   doubles as the regression test for that issue.

## Validating the validator

- Stage-3 gate logic (esp. the fs/2 alias check) can be unit-checked
  **hardware-free** on the ka9q `sig_gen` rig: a `sig_gen` sweep has *no* fs/2
  alias, so gate 4 must FAIL there (good negative control), while a real
  RX888 must PASS — confirming the gate actually discriminates.
- Full run: on the bench with the RX888, `tests/validate.sh` end to end.

## Open / decisions resolved

- Gate on fs/2 alias: **yes** (decision recorded above, with the caveat).
- Packaging: **single `validate.sh` wrapper** (decision).
- Soak duration in the wrapper: configurable; default short, with a note that
  real validation uses a long soak (or run Stage 2 standalone for hours).
