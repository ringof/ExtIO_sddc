# tests/bench — local HITL bench harness

Implements the staged goal ladder from `docs/local-hwil-plan.md`. Each test is
a small, independently-verifiable check that prints one grep-stable verdict
line and exits 0/1 — the contract a local Claude Code `/goal` reads.

## Quick start

```sh
tests/bench/bench.sh list          # show available tests
tests/bench/bench.sh g0-ft8       # run a single test
tests/bench/bench.sh all           # run all in dependency order
```

## Prerequisites

Debian/Ubuntu — install once; the scripts clone+build the pinned encoder/
decoder sources under `.build/` on first run:

```sh
sudo apt-get install -y gcc gfortran libfftw3-dev libfftw3-single3 libgfortran5 sox git
```

Independent operator verification (optional) uses WSJT-X tools (`jt9`,
`wsprd`): install the `wsjtx` package.

## Tests & tools

| Script | Purpose | Hardware |
|---|---|---|
| `run_g0_ft8.sh` | **G0 (FT8)** — off-hardware audio encode→decode self-test | none |
| `run_g0_wspr.sh` | **G0 (WSPR)** — off-hardware audio encode→decode self-test | none |
| `gen_ft8_wav.sh` | generate a known-content FT8 audio WAV | none |
| `gen_wspr_wav.sh` | generate a known-content WSPR audio WAV | none |
| `cat_test.py` | **CAT** — QDX CAT serial: freq set/read, PTT cycle, IF cross-check | QDX |
| `audio_test.py` | **AUDIO** — QDX USB audio: capture, playback via hw:, PTT+play | QDX |
| `rf_test.py` | Manual RF verification: tone + PTT for external receiver/powers | QDX |
| `loopback_test.py` | **LOOPBACK** — CW carrier in powers spectrum via RF loopback | QDX + RX888 |
| `ft8_test.py` | **FT8** — end-to-end FT8 decode across 80/40/30/20m | QDX + RX888 |
| `wspr_test.py` | **WSPR** — end-to-end WSPR decode on 40m | QDX + RX888 |

### Helpers (imported by test scripts)

| Module | Purpose |
|---|---|
| `qdx_cat.py` | QDX Kenwood CAT serial control (FA, TX, RX, TQ, IF, ID, VN) |
| `qdx_audio.py` | QDX ALSA device discovery, tone generation (S24 stereo 48 kHz), play/capture |

### Runner

| Script | Purpose |
|---|---|
| `bench.sh` | Unified test dispatcher — host vs container, single test or `all` |
| `run_cat.sh` | Preflight wrapper for `cat_test.py` |
| `run_audio.sh` | Preflight wrapper for `audio_test.py` |

## bench.sh

The unified runner dispatches tests to the right execution environment:

- **Host tests** (`g0-ft8`, `g0-wspr`, `cat`, `audio`): run directly on the host.
- **Container tests** (`loopback`, `ft8`, `wspr`): run via `docker exec` in the
  ka9q-radio container (override with `CONTAINER=` env var).

```sh
bench.sh cat                       # run CAT test on host
bench.sh ft8 --message "CQ T1ABC FN20"  # run FT8 in container with args
CONTAINER=my-radio bench.sh loopback     # override container name
bench.sh all                       # run everything, print summary
```

## Encoders, decoders, and sox's role

Real tools own the protocol encoding and decoding; **sox only converts sample
rate / format** — it never synthesizes a signal.

| Mode | Encoder (audio) | Native rate | Decoder | Decoder input |
|---|---|---|---|---|
| FT8 | `gen_ft8` (ft8_lib, pinned) | 12 kHz mono | `decode_ft8` (ft8_lib) | 12 kHz |
| WSPR | `wsprsimwav` (wspr-cui, pinned) | 48 kHz mono | `wsprd` (wspr-cui) | 12 kHz |

The native rates are convenient: **48 kHz** is the QDX soundcard TX rate,
**12 kHz** is what the decoders want — `sox` bridges either direction.

External sources are git-cloned and built under `.build/` on first run, pinned
to specific commits (mirroring the ka9q-radio SHA pin in the Dockerfile):

- `ft8_lib` — needs `gcc`.
- `wspr-cui` — needs `gfortran` + `libfftw3-dev` (build) and `libgfortran5` +
  `libfftw3-single3` (runtime).
- `sox` for resampling.

## G0 (FT8 / WSPR)

Each `run_g0_*.sh` proves its audio toolchain with **no hardware**:

1. **Fixture check (authoritative):** decodes each `fixtures/<mode>/<name>.wav`
   (resampling to the decoder's rate as needed) and asserts the message in its
   sibling `<name>.expected`.
2. **Self-loop check (mechanics):** encode→decode round-trip of `G0_MSG`. Proves
   plumbing even before fixtures exist (same-tool, so not a correctness
   authority on its own).

```sh
tests/bench/run_g0_ft8.sh    # -> "G0 FT8 OK ..."  | "G0 FT8 FAIL — ..."
tests/bench/run_g0_wspr.sh   # -> "G0 WSPR OK ..." | "G0 WSPR FAIL — ..."
```

Both are hardware-free and intended to also run in hosted CI as decoder
regression tests.

### Outputs (for independent verification)

Every run writes its rendered audio to `out/` (gitignored) so you have a real
file to inspect and decode **with your own tools** — that's how you check this
work without trusting the harness's own verdict:

```
out/g0_ft8_selfloop.wav        # jt9 -8 out/g0_ft8_selfloop.wav
out/g0_wspr_selfloop_48k.wav   # 48 kHz, the QDX TX rate
out/g0_wspr_selfloop_12k.wav   # wsprd out/g0_wspr_selfloop_12k.wav
```

## QDX tests (CAT, AUDIO) and manual RF test

All QDX scripts take `--port` (default `/dev/ttyACM0`) and `--help`.

```sh
# CAT — serial control
python tests/bench/cat_test.py
python tests/bench/cat_test.py --port /dev/ttyACM1 --freq-offset 500

# AUDIO — USB audio path (capture, playback, PTT)
python tests/bench/audio_test.py
python tests/bench/audio_test.py --port /dev/ttyACM0 --tone 1000 --duration 3

# Manual RF verification — long tone for powers / external receiver
python tests/bench/rf_test.py --freq 10000000 --duration 30
```

## Generating audio (TX stimulus / fixtures)

```sh
# FT8: 12 kHz mono (decoder rate). 3rd arg = audio Hz within the passband.
tests/bench/gen_ft8_wav.sh  "CQ T1ABC FN20" cq.wav 1500

# WSPR: 48 kHz mono native (QDX TX rate); pass a rate to resample via sox.
tests/bench/gen_wspr_wav.sh "T1ABC FN20 30" wspr48.wav          # 48 kHz
tests/bench/gen_wspr_wav.sh "T1ABC FN20 30" wspr12.wav 12000    # 12 kHz
```

These same encoders are the TX stimulus source for the on-bench tests (FT8, WSPR).

## Adding an authoritative fixture

Drop a known-content WAV under `fixtures/ft8/` or `fixtures/wspr/` with a
sibling `.expected` file holding the exact decoded message, e.g.:

```
fixtures/wspr/t1abc.wav
fixtures/wspr/t1abc.expected   # contains: T1ABC FN20 30
```

The harness auto-discovers it (resampling as needed) — no code change. Keep
clips small; a 48 kHz/112 s WSPR WAV is ~10 MB, so prefer 12 kHz or `git-lfs`
for committed WSPR audio.
