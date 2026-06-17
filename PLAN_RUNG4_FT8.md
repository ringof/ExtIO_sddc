# Plan: Rung 4 — End-to-end FT8 decode across 80/40/30/20m

## Context

The HITL bench ladder has proven: encode/decode toolchain (G0), QDX CAT
control (2a), QDX audio path (2b), and CW carrier in spectrum (3). The next
step is proving the full FT8 decode path over RF: QDX transmits a known FT8
message → attenuated cable → RX888 → radiod demodulates via [FT8] channel →
pcmrecord captures slot-aligned WAV → decode_ft8 asserts the known message.

This collapses the original plan's rungs 4 (off-hardware fixture) and 5
(live RF) into a single end-to-end script. The script sweeps all four QDX
bands (80→40→30→20m) to exercise the QDX VFO across its range.

## Files to create/modify

| Action | File | What |
|--------|------|------|
| NEW    | `tests/bench/rung4_ft8_test.py` | Orchestrator script |
| MODIFY | `docker/ka9q-radio/Dockerfile`  | Bake ft8_lib (gen_ft8 + decode_ft8) into image |

## Dockerfile changes

Add to the **builder** stage, after the ka9q-radio build:

```dockerfile
# Build ft8_lib encode/decode tools (pinned for reproducibility).
ARG FT8_LIB_SHA=9fec6ca39886edbf96f4f5e71edc76da5074e871
RUN git clone https://github.com/kgoba/ft8_lib.git /build/ft8_lib \
    && git -C /build/ft8_lib checkout "${FT8_LIB_SHA}" \
    && make -C /build/ft8_lib gen_ft8 decode_ft8
```

Add to the **runtime** stage COPY section:

```dockerfile
COPY --from=builder /build/ft8_lib/gen_ft8 /usr/local/bin/
COPY --from=builder /build/ft8_lib/decode_ft8 /usr/local/bin/
```

## Orchestrator script: `rung4_ft8_test.py`

### Band table

```python
BANDS = [
    {"name": "80m", "dial_hz": 3_573_000, "ssrc": 3573},
    {"name": "40m", "dial_hz": 7_074_000, "ssrc": 7074},
    {"name": "30m", "dial_hz": 10_136_000, "ssrc": 10136},
    {"name": "20m", "dial_hz": 14_074_000, "ssrc": 14074},
]
```

### Unique message generation

Each run generates a unique FT8 message to guarantee no cached/stale
decode can false-positive. The message is generated once per run and
reused for all four bands.

```python
import random, string

def random_grid():
    """Random Maidenhead grid square (e.g. 'JO31')."""
    return (random.choice(string.ascii_uppercase[:18])
          + random.choice(string.ascii_uppercase[:18])
          + str(random.randint(0, 9))
          + str(random.randint(0, 9)))

def random_callsign():
    """Random realistic US ham callsign (e.g. 'WA3KRT')."""
    prefix = random.choice(['W', 'K', 'N', 'AA', 'AK', 'WA', 'KB'])
    region = str(random.randint(0, 9))
    suffix = ''.join(random.choices(string.ascii_uppercase,
                                    k=random.choice([1, 2, 3])))
    return f"{prefix}{region}{suffix}"

# Once per run:
message = f"CQ {random_callsign()} {random_grid()}"
# e.g. "CQ WA3KRT JO47"
```

### Arguments (argparse, following rung3 pattern)

- `--port` — QDX serial port (default `/dev/ttyACM0`)
- `--baud` — baud rate (default 9600)
- `--card` — ALSA card number (default: auto-discover)
- `--group` — pcmrecord data group (default `ft8-pcm.local`)
- `--message` — override the auto-generated FT8 message (default: random per run)
- `--tone-freq` — audio frequency in Hz for gen_ft8 (default 1500)

### Per-band flow

For each band in BANDS:

1. **Generate TX audio** (once, before the band loop — all use the same
   message, only dial changes):
   - `gen_ft8 "{message}" /tmp/rung4_ft8_12k.wav 1500`
   - `sox /tmp/rung4_ft8_12k.wav -r 48000 -c 2 -b 24 /tmp/rung4_ft8_tx.wav`
   - The 12 kHz→48 kHz stereo S24 upsample happens once; reused for all bands.
   - `{message}` is the randomly generated string (e.g. `CQ WA3KRT JO47`).

2. **Set QDX frequency** — `qdx.set_freq(dial_hz)`, verify readback.

3. **Start pcmrecord** in background:
   ```
   pcmrecord -8 --ssrc {ssrc} -d /tmp/rung4_caps/{band} ft8-pcm.local
   ```
   - `-8` = JT FT8 mode: 15s max length, slot-aligned, padded
   - `--ssrc {ssrc}` selects only the channel for this band's frequency
   - Writes files like `YYYYMMDDTHHMMSSz_FREQ_usb.wav`

4. **Wait for next FT8 slot boundary**:
   ```python
   def wait_for_slot(period_s=15):
       """Sleep until the next FT8 slot boundary (:00/:15/:30/:45)."""
       now = time.time()
       elapsed = now % period_s
       remaining = period_s - elapsed
       if remaining < 2.0:       # too close to boundary, wait for next
           remaining += period_s
       time.sleep(remaining)
   ```

5. **TX: key QDX + play audio**:
   - `qdx.tx_on()` — verify TX state via `get_tx_state()`
   - `aplay -D hw:{card},0 /tmp/rung4_ft8_tx.wav` — in subprocess
   - FT8 audio is ~13s; aplay finishes before the 15s slot ends
   - Wait for aplay to complete (timeout: 20s)
   - `qdx.tx_off()`

6. **Wait for pcmrecord to close the file**: sleep until slot boundary
   + 2s grace (pcmrecord closes the file at Max_length = 15s from slot
   start).

7. **Stop pcmrecord** — `SIGTERM` the background process.

8. **Find the captured WAV**: glob `/tmp/rung4_caps/{band}/*.wav`,
   take the most recent file. Fail if none found.

9. **Decode + assert**:
   - `decode_ft8 <captured.wav>` — capture stdout
   - `grep -F {message}` in the output
   - Check: message decoded = 1 check per band

### Verdict

```
RUNG4 FT8 OK (4 bands: 80m 40m 30m 20m)
RUNG4 FT8 FAIL — 20m: "{message}" not decoded
```

Exit 0 on all bands pass, exit 1 on any failure. Per-band progress
printed as it runs (grep-stable per-band lines + overall verdict).

### Error handling / cleanup

- Wrap the band loop in try/finally to ensure `qdx.tx_off()` on any
  exception (same pattern as rung3's QdxCat context manager)
- Kill pcmrecord if still running on exception
- Restore QDX to its original frequency after the full sweep
- Clean up temp directories on success (keep on failure for debugging)

### Reused code

- `qdx_cat.py` — `QdxCat` context manager (set_freq, tx_on, tx_off, get_tx_state)
- `qdx_audio.py` — `find_qdx_card()`, `qdx_hw_device()`
- gen_ft8 / decode_ft8 — baked into image (no runtime build)
- sox — already in image (audio format conversion)
- pcmrecord — already added to image in previous commit

## Verification

1. Build image: `docker build -f docker/ka9q-radio/Dockerfile -t ka9q-radio .`
   — must succeed (compiles ft8_lib + pcmrecord + ka9q-radio)
2. Start container with QDX + RX888 connected, radiod streaming
3. Run: `python3 /usr/local/lib/bench/rung4_ft8_test.py`
4. Expected: per-band decode lines + `RUNG4 FT8 OK (4 bands: 80m 40m 30m 20m)`
5. Regression: `tests/ka9q_smoke.sh` still passes (channels are additive)

## TODO: Rename bench test scripts to functional names

The `rungN_` prefix reflects build order, not function. Before merging
or after the ladder is complete, rename to describe what each test does:

| Current | New name | What it tests |
|---------|----------|---------------|
| `rung2a_cat_test.py` | `qdx_cat_test.py` | QDX CAT serial control |
| `rung2b_audio_test.py` | `qdx_audio_test.py` | QDX USB audio path |
| `rung3_loopback_test.py` | `rf_carrier_test.py` | CW carrier in spectrum |
| `rung4_ft8_test.py` | `ft8_roundtrip_test.py` | FT8 TX→RF→decode across bands |

Future WSPR test: `wspr_roundtrip_test.py`.

Touch points: filenames, Dockerfile COPY lines, `docs/local-hwil-plan.md`,
`tests/bench/README.md`, any cross-references in bench scripts.
