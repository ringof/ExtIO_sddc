# Local Hardware-in-the-Loop (HWIL) Test Bench — Plan 1

Status: **Approved; in progress.**

- **G0** — implemented and committed (PR #172): off-hardware FT8 + WSPR audio
  encode→decode self-tests and audio generators.
- **2a** — QDX CAT serial control: all commands verified on real hardware
  (FA, IF, ID, VN, TX, RX, TQ). Firmware 1.09.
- **2b** — QDX USB audio path: S24 stereo 48 kHz via hw: (no plughw:),
  capture with energy check, PTT+playback. RF output confirmed by operator
  using independent HF receiver.
- **3** — CW carrier confirmed in `powers` spectrum: QDX TX at 10 MHz +
  1500 Hz tone, 2W through 120 dB attenuation, peak at -82.7 dB with
  53 dB SNR in 1 Hz bins. Verified manually (not yet automated).
- Rungs 1, 4–7 pending.

## Purpose

Build a **local** closed-RF test bench that exercises the *entire* real
signal path of this firmware — QDX transmitter → attenuators → RX888 →
FX3 firmware (this repo) → `radiod` (ka9q-radio) → decoder → decoded
message — and that a **local Claude Code instance can drive as a `/goal`**.

The bench produces a single, unambiguous, machine-parseable pass/fail line.
That line is what makes a `/goal` honest: the `/goal` evaluator only judges
what is surfaced in the transcript, and here the surfaced line *is* the
output of a real RF decode.

### What this validates (and what it does not)

- **Validates:** firmware build correctness in-circuit, ADC/clock/GPIF/USB
  path, the `radiod` `rx888.so` driver path, and end-to-end demodulation of
  a known signal on real hardware.
- **Does NOT validate:** antenna, propagation, or real-world RFI. This is a
  *wired* bench at a controlled level. A green bench means "the firmware and
  its software path work," not "validated like a live station."

## Explicitly out of scope (deferred to Plan 2)

- Self-hosted GitHub Actions runner.
- GitHub Actions / `workflow_dispatch` / scheduled CI integration.
- The runner **security model** (untrusted-fork code execution on a machine
  wired to a transmitter, network segmentation, ephemeral runners, required
  reviewers). This surface is large enough to warrant its own plan and must
  not be hand-waved as a subsection here.

Plan 1 assumes a trusted local operator on a trusted local machine.

## Two phases

The two phases **share the entire bench** (QDX TX head, attenuator chain,
RX888 → `radiod`). Only the TX mode and the decoder tail differ; the
orchestrator is parameterized by `MODE={ft8,wspr}`.

### Phase A — FT8, the fast iteration loop

- 15 s T/R cycle aligned to `:00/:15/:30/:45` → one transmit-and-decode
  iteration in ~15–30 s. This is the loop a local Claude Code instance
  iterates on while changing firmware.
- **Encode + decode via `ft8_lib`** (kgoba): `gen_ft8` produces the TX WAV,
  `decode_ft8` decodes a captured 15 s WAV. One small C dependency covers
  both head and tail; trivially containerized; no GUI.
- Known structured message, e.g. `CQ T1ABC FN20`, so the expected decode is
  known a priori.

`/goal` (dev cadence):

```
/goal the ft8-loopback harness prints "FT8 DECODE OK" for CQ T1ABC FN20
      on 20m in >=2 of 4 consecutive slots, or stop after 6 runs
```

### Phase B — WSPR via real wsprdaemon, the sign-off test

- Deployment-representative: the decoder tail is the **actual** wsprdaemon →
  `wsprd` → spot pipeline that the community running this firmware uses. It
  exercises things `ft8_lib` never touches (2-minute integration,
  wsprdaemon's own ka9q-radio ingestion/band handling, the full spot record:
  callsign / grid / dBm / freq / SNR / drift).
- Slow (2-minute, even-minute aligned), so it is a final/sign-off check, not
  a per-iteration one.

`/goal` (sign-off cadence):

```
/goal wsprdaemon emits a WSPR spot for T1ABC/FN20 within 5 Hz and the
      spot's grid+power fields match the transmitted message,
      or stop after 4 attempts
```

## Goal ladder (staged)

Decompose the two phases into small, independently-verifiable rungs. Each
rung is a single `/goal` (one measurable verdict line + a turn bound) and a
natural commit / sub-issue under #171. A rung's predecessor must be green
before it starts.

Hardware dependency is called out because the off-hardware rungs (**G0, 4,
6**) can be developed and regression-tested anywhere — including in hosted CI
(`build.yml`), not just on the bench.

| Rung | What it proves | RX888 | QDX | RF/TX |
|---|---|:--:|:--:|:--:|
| **G0** | audio encode↔decode self-test; emits out/ artifacts to decode | – | – | – |
| **1** | HITL image (built `FROM` the ka9q-radio image) reproduces the existing smoke-test output with an RX888 attached | ✓ | – | – |
| **2a** | QDX CAT control: set freq / key PTT / read response (serial) | – | ✓ | – |
| **2b** | QDX audio path: TX audio injected via the USB soundcard | – | ✓ | – |
| **3** | CW carrier near 10 MHz appears in the `powers` spectrum | ✓ | ✓ | ✓ |
| **4** | FT8 decode automation: orchestrator decodes the G0 fixture and emits its verdict line | – | – | – |
| **5** | TX/RX FT8 end-to-end over the bench | ✓ | ✓ | ✓ |
| **6** | WSPR decode automation (wsprdaemon) on a known fixture | – | – | – |
| **7** | TX/RX WSPR end-to-end over the bench | ✓ | ✓ | ✓ |

Per-rung specifics:

- **Rung 1:** the HITL image is built `FROM` the existing `ka9q-radio` image
  (or shares its builder stage) so smoke-test parity is structural and cannot
  drift. Success = reproduces the current smoke output.
- **Rung 3 assertion (numeric):** transmit a steady single audio tone → a
  single RF carrier near **10 MHz** (reuses the WWV 10 MHz channel already in
  `rx888-test.conf`). PASS = `powers` shows a peak **within 300 Hz** of the
  expected frequency, **≥ 20 dB above the noise floor**. ("CW" here = a
  constant tone → constant carrier; the QDX is a constant-envelope digital
  transceiver, not a Morse keyer.)
- **Rungs 4 & 6** assert against generated known-content fixtures (see Audio
  fixtures) — no transmit, no hardware. These belong in hosted CI as decoder
  regression tests, pinning the decoder + ka9q-radio versions to known output.

### RF safety pre-gate (rungs 3, 5, 7)

The first time the QDX is keyed into the RX888 is the destructive-risk
moment. Every rung that transmits into the RX888 (3, 5, 7) MUST print a
**red `DANGER!!` banner** before keying — reminding the operator that the
first attenuator stage must be power-rated for full QDX output and the level
at the RX888 input must be in range. This is an operator pre-flight that
*gates* the rung; it is not discovered by it.

## Audio generation, fixtures, and independent verification

We generate the FT8/WSPR audio ourselves with the real protocol tools, so
**ground truth is known by construction** — we choose the message, so the
expected decode is authoritative.

Tooling (validated; see `tests/bench/`):

- **FT8:** `gen_ft8` / `decode_ft8` (`ft8_lib`@9fec6ca) — 12 kHz mono audio.
- **WSPR:** `wsprsimwav` (`wspr-cui`@839b86f, WSJT-X 2.7.1-based) renders
  48 kHz mono audio with raised-cosine shaping; `wsprd` decodes (12 kHz input).
- **`sox`** does sample-rate/format conversion only (48k↔12k) — never signal
  synthesis.

The encoders do double duty: the same `gen_ft8` / `wsprsimwav` that make the
test audio are the TX stimulus source for the on-bench rungs (5, 7).

**Independence model — the operator is the verifier.** Encoder independence
was intentionally *waived* (the same tool family encodes and decodes).
Verification independence comes from the human instead: every G0 run writes
its rendered audio to `tests/bench/out/` (gitignored) and prints a decode
command, so the operator confirms the result with their **own** tools — e.g.
`jt9 -8 out/g0_ft8_selfloop.wav` (WSJT-X `jt9`, an independent codebase from
`ft8_lib`) or `wsprd out/g0_wspr_selfloop_12k.wav`. The harness verdict is a
check the operator re-runs, not a claim to trust. On the hardware rungs this
is stronger still: the transmitter and the receive/measurement chain are
independent systems coupled only by RF.

**Committed fixtures vs on-the-fly.** A small FT8 fixture
(`fixtures/ft8/cq_t1abc.wav`, 12 kHz, ~350 KB) is committed so rung 4 has a
fixed regression input; WSPR audio is generated on the fly each run (a 48 kHz
WSPR clip is ~11 MB — too large to commit) and surfaced via `out/`. Each
harness auto-discovers any `fixtures/<mode>/<name>.wav` + `.expected` and
resamples as needed.

## Bench topology

```
[QDX]  -- PC: encode + CAT/PTT -->  TX RF (~+37 dBm at full drive)
  |
  +--> [attenuator chain; FIRST STAGE power-rated for full QDX output]
         |
         +--> ~ -40 dBm  -->  [RX888 antenna input]
                                |
                                +--> FX3 firmware (this repo) --> USB3
                                       |
                                       +--> radiod (rx888.so) --> multicast I/Q
                                              |
                                              +--> decoder tail (ft8_lib | wsprdaemon)
                                                     |
                                                     +--> SPOT / DECODE line
```

## Interface contract (operator must supply before build)

These cannot be guessed; the orchestrator and safety posture depend on them.

- **RF safety (non-negotiable):**
  - Total attenuation (dB).
  - **Power rating of the first attenuator stage** — the QDX puts ~5 W
    (+37 dBm) into stage one; a mis-rated first stage destroys the RX888
    front end. State target level at the RX888 input (e.g. −40 dBm).
- **QDX:** USB sound-device name (ALSA), CAT serial port + baud, band/dial
  freq, drive level, firmware version.
- **RX888 / radiod:** WSPR/FT8 band(s), sample-rate/center config (extends
  `docker/ka9q-radio/rx888-test.conf`, currently 64.8 Msps direct sampling).
- **Pass criteria:** test callsign, grid, freq tolerance (Hz), min SNR, time
  window, retry/quorum count (e.g. N-of-M slots).

## Software additions to the docker harness

Extends the existing `docker/ka9q-radio/` image, which already builds
`radiod` + `rx888.so` + `sig_gen.so` and flashes `SDDC_FX3.img`.

1. **Receive capture:** capture a fixed window of audio/IQ from the `radiod`
   channel (ka9q-radio `pcmrecord`/`monitor` to WAV).
2. **FT8 tail:** build `ft8_lib`; `decode_ft8` on the captured WAV.
3. **WSPR tail:** add `wsprdaemon` + `wsprd`, wired to ingest from `radiod`
   (native ka9q-radio support).
4. **TX head service:** on command, encode the chosen message (`gen_ft8` for
   FT8; WSPR encoder for WSPR), play into the QDX ALSA device, key PTT via
   CAT serial.
5. **Orchestrator** (`MODE={ft8,wspr}`): align to the slot boundary →
   trigger TX with the known message → capture the slot → decode → grep for
   the expected callsign within tolerance → emit exactly one line
   (`FT8 DECODE OK ...` / `WSPR SPOT ...` / `NO DECODE`) → exit 0/1.

## Operational requirements for an unattended `/goal`

A local `/goal` runs the bench repeatedly without a human babysitting each
turn, so the bench must be self-recovering and bounded:

- **Clock discipline:** NTP-synced host; both FT8 and WSPR are slot-aligned.
- **Self-reset between runs:** USB power-cycle of RX888/QDX (e.g. `uhubctl`),
  re-flash to known DFU state, restart `radiod`.
- **Hard timeout / watchdog:** a wedged FX3 or stuck `radiod` fails loud
  rather than hanging the `/goal` loop.
- **Single-line verdict:** the orchestrator's terminal line is the contract
  the `/goal` evaluator reads — it must be unambiguous and grep-stable.

## Closed-system etiquette / legality

- **No public spots.** wsprdaemon / FT8 decode runs with **network upload
  disabled** and a reserved test callsign. We inject synthetic `T1ABC`
  signals into a load — uploading them to wsprnet.org / PSKReporter would
  pollute public databases. Closed bench only.
- **RF containment.** Transmit into the attenuator/load chain; no antenna.
  Transmitting into a load is not radiating, but the plan states containment
  explicitly.

## Deliverables

- `docker/ka9q-radio/` extended: FT8 (`ft8_lib`) + WSPR (`wsprdaemon`) tails,
  capture tooling, TX head service.
- `tests/` (or `bench/`) orchestrator script, `MODE`-parameterized, emitting
  the single-line verdict.
- An interface-contract doc the operator fills in (attenuation/power rating,
  QDX/RX888 specifics, pass criteria).
- This plan document.

## Open questions for the operator

1. Initial band for Phase A FT8 (default 20 m, 14.074 MHz dial)?
2. ~~Reserved test callsign + grid~~ — **resolved: `T1ABC` / `FN20`** (used
   throughout G0).
3. Is a USB-switchable hub (`uhubctl`-capable) available for self-reset, or
   should reset fall back to software-only (DFU re-flash + `radiod` restart)?
4. Sibling-container split (TX head vs RX/decoder) or single container?

## Next step

Rungs G0, 2a, 2b proven on real hardware; rung 3 (CW carrier in `powers`)
confirmed manually.  Next: **automate rung 3** as a scripted test, then
**rung 4** (FT8 decode automation — off-hardware) and **rung 5** (TX/RX FT8
end-to-end over the bench).  Rung 1 (HITL image parity) can proceed in
parallel.  Plan 2 (remote HWIL CI + security) is written separately.
