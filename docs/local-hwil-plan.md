# Local Hardware-in-the-Loop (HWIL) Test Bench — Plan 1

Status: **DRAFT — awaiting approval**. No build code is written until this
plan is approved.

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
2. Reserved test callsign + grid to standardize on (placeholder: `T1ABC` /
   `FN20`)?
3. Is a USB-switchable hub (`uhubctl`-capable) available for self-reset, or
   should reset fall back to software-only (DFU re-flash + `radiod` restart)?
4. Sibling-container split (TX head vs RX/decoder) or single container?

## Next step after approval

On approval, implement Phase A first (FT8 fast loop end-to-end), validate it
drives a local `/goal` to `FT8 DECODE OK`, then add Phase B (WSPR sign-off).
Plan 2 (remote HWIL CI + security) is written separately.
