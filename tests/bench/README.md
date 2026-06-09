# tests/bench — local HITL bench harness

Implements the staged goal ladder from `docs/local-hwil-plan.md`. Each rung is
a small, independently-verifiable check that prints one grep-stable verdict
line and exits 0/1 — the contract a local Claude Code `/goal` reads.

## Prerequisites

Debian/Ubuntu — install once; the scripts clone+build the pinned encoder/
decoder sources under `.build/` on first run:

```sh
sudo apt-get install -y gcc gfortran libfftw3-dev libfftw3-single3 libgfortran5 sox git
```

Independent operator verification (optional) uses WSJT-X tools (`jt9`,
`wsprd`): install the `wsjtx` package.

## Rungs & tools

| Script | Purpose | Hardware |
|---|---|---|
| `run_g0_ft8.sh` | **G0 (FT8)** — off-hardware audio encode→decode self-test | none |
| `run_g0_wspr.sh` | **G0 (WSPR)** — off-hardware audio encode→decode self-test | none |
| `gen_ft8_wav.sh` | generate a known-content FT8 audio WAV | none |
| `gen_wspr_wav.sh` | generate a known-content WSPR audio WAV | none |

(Rungs 1–7 are added as the ladder is climbed; see the plan doc.)

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

## Generating audio (TX stimulus / fixtures)

```sh
# FT8: 12 kHz mono (decoder rate). 3rd arg = audio Hz within the passband.
tests/bench/gen_ft8_wav.sh  "CQ T1ABC FN20" cq.wav 1500

# WSPR: 48 kHz mono native (QDX TX rate); pass a rate to resample via sox.
tests/bench/gen_wspr_wav.sh "T1ABC FN20 30" wspr48.wav          # 48 kHz
tests/bench/gen_wspr_wav.sh "T1ABC FN20 30" wspr12.wav 12000    # 12 kHz
```

These same encoders are the TX stimulus source for the on-bench rungs (5/7).

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
