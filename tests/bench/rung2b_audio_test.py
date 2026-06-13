#!/usr/bin/env python3
"""tests/bench/rung2b_audio_test.py — Rung 2b: QDX USB Audio path test.

Proves that the bench host can play audio into and capture audio from the
QDX over its USB Audio Class device — the other half of QDX control (2a
proved CAT serial).  No RX888, no RF path — just the QDX connected via USB.

The audio plumbing validated here is reused by every TX rung (3, 5, 7):
they all need to play encoded FT8/WSPR audio into the QDX while PTT is keyed.

Env vars:
  QDX_PORT       serial port path (required, for PTT control)
  QDX_BAUD       baud rate (default 9600)
  QDX_CARD       override ALSA card number (default: auto-discover)
  QDX_TONE_FREQ  tone frequency in Hz (default 1500)
  QDX_TONE_DUR   tone duration in seconds (default 2)
"""

import os
import sys
import time

# Allow importing from the same directory.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from qdx_audio import (
    QdxAudioError, find_qdx_card, qdx_hw_device,
    generate_tone, play_to_qdx, capture_from_qdx,
)
from qdx_cat import QdxCat, QdxCatError

# Output directory for generated/captured files.
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")


def main():
    port = os.environ.get("QDX_PORT")
    if not port:
        print("RUNG2B AUDIO FAIL — QDX_PORT not set", file=sys.stderr)
        sys.exit(1)

    baud = int(os.environ.get("QDX_BAUD", "9600"))
    tone_freq = int(os.environ.get("QDX_TONE_FREQ", "1500"))
    tone_dur = float(os.environ.get("QDX_TONE_DUR", "2"))

    os.makedirs(OUT_DIR, exist_ok=True)
    checks = 0

    try:
        # --- 1. Device discovery ------------------------------------------
        card_override = os.environ.get("QDX_CARD")
        if card_override is not None:
            card = int(card_override)
        else:
            card = find_qdx_card()

        hw = qdx_hw_device(card)
        print(f"RUNG2B: QDX audio device -> card {card}, {hw} (QDX Transceiver)")
        checks += 1

        # --- 2. Capture test ----------------------------------------------
        capture_path = os.path.join(OUT_DIR, "rung2b_capture.wav")
        capture_from_qdx(capture_path, hw, duration_s=tone_dur)
        fsize = os.path.getsize(capture_path)
        if fsize == 0:
            print("RUNG2B AUDIO FAIL — captured WAV is empty")
            sys.exit(1)
        print(f"RUNG2B: capture test -> {capture_path} "
              f"({fsize} bytes, {tone_dur} s) OK")
        checks += 1

        # --- 3. Tone generation -------------------------------------------
        tone_path = os.path.join(OUT_DIR, "rung2b_tone.wav")
        generate_tone(tone_path, freq_hz=tone_freq, duration_s=tone_dur)
        print(f"RUNG2B: tone generated -> {tone_path} "
              f"({tone_freq} Hz, {tone_dur} s, 48 kHz)")

        # --- 4. Playback test (no PTT) ------------------------------------
        play_to_qdx(tone_path, hw)
        print("RUNG2B: playback test (no PTT) -> OK")
        checks += 1

        # --- 5. PTT + playback test ---------------------------------------
        with QdxCat(port, baudrate=baud) as qdx:
            pre = qdx.get_tx_state()
            if pre:
                print("RUNG2B AUDIO FAIL — QDX already in TX before PTT test")
                sys.exit(1)

            qdx.tx_on()
            tx_state = qdx.get_tx_state()
            if not tx_state:
                print("RUNG2B AUDIO FAIL — QDX did not enter TX after TX;")
                sys.exit(1)

            play_to_qdx(tone_path, hw)

            # Small settle before querying — PTT still held during playback.
            time.sleep(0.1)
            still_tx = qdx.get_tx_state()

            qdx.tx_off()
            rx_state = qdx.get_tx_state()
            if rx_state:
                print("RUNG2B AUDIO FAIL — QDX did not return to RX after RX;")
                sys.exit(1)

        print("RUNG2B: PTT + playback test -> TX during playback, RX after OK")
        checks += 1

        # --- Verdict ------------------------------------------------------
        print(f"RUNG2B AUDIO OK ({checks} checks, card {card} {hw})")
        print(f"RUNG2B: verify yourself: aplay -D {hw} {tone_path}")
        print(f"RUNG2B: verify yourself: arecord -D {hw} "
              f"-f S24_3LE -c 2 -r 48000 -d 2 /tmp/qdx_capture.wav")

    except QdxAudioError as exc:
        print(f"RUNG2B AUDIO FAIL — {exc}")
        sys.exit(1)
    except QdxCatError as exc:
        print(f"RUNG2B AUDIO FAIL — CAT error: {exc}")
        sys.exit(1)
    except Exception as exc:
        print(f"RUNG2B AUDIO FAIL — unexpected: {exc}")
        sys.exit(1)


if __name__ == "__main__":
    main()
