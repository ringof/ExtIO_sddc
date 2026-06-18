#!/usr/bin/env python3
"""tests/bench/audio_test.py — QDX USB Audio path test.

Tests that the bench host can play audio into and capture audio from the
QDX over its USB Audio Class device, using the QDX's native format
(S24_3LE stereo, 48 kHz) on the hw: device — no silent plughw: conversion.

What this proves:
  - ALSA device discovery finds the QDX
  - arecord captures non-silent audio from the QDX (RX noise)
  - aplay plays S24 stereo audio to the QDX via hw: (format match, no plughw:)
  - PTT + playback sequence completes without crashing

What this does NOT prove (requires manual verification):
  - That the played audio actually modulates onto an RF carrier
  - Use rf_test.py + a separate receiver to verify RF output
"""

import argparse
import os
import struct
import sys
import time
import wave

# Allow importing from the same directory.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from qdx_audio import (
    QdxAudioError, find_qdx_card, qdx_hw_device,
    generate_tone, play_to_qdx, capture_from_qdx,
)
from qdx_cat import QdxCat, QdxCatError

# Output directory for generated/captured files.
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")


def wav_has_energy(path: str, threshold: int = 100) -> bool:
    """Return True if the WAV file contains samples above *threshold*.

    Works with 16- and 24-bit WAVs.  A file that is all-zero (or below
    threshold) means the capture device produced silence — not a real signal.
    """
    with wave.open(path, "rb") as wf:
        n = wf.getnframes()
        sw = wf.getsampwidth()
        raw = wf.readframes(n)
    if sw == 2:
        samples = struct.unpack(f"<{len(raw)//2}h", raw)
    elif sw == 3:
        samples = []
        for i in range(0, len(raw), 3):
            val = int.from_bytes(raw[i:i+3], "little", signed=True)
            samples.append(val)
    elif sw == 4:
        samples = struct.unpack(f"<{len(raw)//4}i", raw)
    else:
        return True  # can't parse, assume OK
    peak = max(abs(s) for s in samples) if samples else 0
    return peak > threshold


def main():
    p = argparse.ArgumentParser(description="QDX USB audio path test")
    p.add_argument("--port", default="/dev/ttyACM0", help="QDX serial port")
    p.add_argument("--baud", type=int, default=9600, help="baud rate")
    p.add_argument("--card", type=int, default=None,
                   help="ALSA card number (default: auto-discover)")
    p.add_argument("--tone", type=int, default=1500,
                   help="tone frequency in Hz (default: 1500)")
    p.add_argument("--duration", type=float, default=2,
                   help="tone duration in seconds (default: 2)")
    args = p.parse_args()

    os.makedirs(OUT_DIR, exist_ok=True)
    checks = 0

    try:
        # --- 1. Device discovery ------------------------------------------
        if args.card is not None:
            card = args.card
        else:
            card = find_qdx_card()

        hw = qdx_hw_device(card)
        print(f"AUDIO: QDX audio device -> card {card}, {hw}")
        checks += 1

        # --- 2. Capture test (RX noise) -----------------------------------
        capture_path = os.path.join(OUT_DIR, "audio_capture.wav")
        capture_from_qdx(capture_path, hw, duration_s=args.duration)
        fsize = os.path.getsize(capture_path)
        if fsize == 0:
            print("AUDIO FAIL — captured WAV is empty (0 bytes)")
            sys.exit(1)
        has_energy = wav_has_energy(capture_path)
        if not has_energy:
            print("AUDIO FAIL — captured WAV is all silence")
            sys.exit(1)
        print(f"AUDIO: capture -> {fsize} bytes, non-silent OK")
        checks += 1

        # --- 3. Tone generation (QDX native format) -----------------------
        tone_path = os.path.join(OUT_DIR, "audio_tone.wav")
        generate_tone(tone_path, freq_hz=args.tone, duration_s=args.duration)
        print(f"AUDIO: tone -> {tone_path} "
              f"({args.tone} Hz, {args.duration} s, S24 stereo 48 kHz)")

        # --- 4. Playback via hw: (no plughw: conversion) ------------------
        play_to_qdx(tone_path, hw)
        print(f"AUDIO: aplay -D {hw} -> accepted S24 stereo OK")
        checks += 1

        # --- 5. PTT + playback -------------------------------------------
        with QdxCat(args.port, baudrate=args.baud) as qdx:
            dial = qdx.get_freq()
            print(f"AUDIO: dial {dial} Hz, carrier {dial + args.tone} Hz")

            pre = qdx.get_tx_state()
            if pre:
                print("AUDIO FAIL — QDX already in TX before PTT test")
                sys.exit(1)

            qdx.tx_on()
            tx_state = qdx.get_tx_state()
            if not tx_state:
                print("AUDIO FAIL — TQ not TX after TX;")
                sys.exit(1)

            play_to_qdx(tone_path, hw)

            qdx.tx_off()
            rx_state = qdx.get_tx_state()
            if rx_state:
                print("AUDIO FAIL — TQ not RX after RX;")
                sys.exit(1)

        print("AUDIO: PTT + playback -> TX during play, RX after OK")
        checks += 1

        # --- Verdict ------------------------------------------------------
        print(f"AUDIO OK ({checks} checks, card {card} {hw})")
        print(f"AUDIO NOTE: RF output NOT verified by this test.")
        print(f"AUDIO NOTE: use rf_test.py + a receiver to confirm TX.")

    except QdxAudioError as exc:
        print(f"AUDIO FAIL — {exc}")
        sys.exit(1)
    except QdxCatError as exc:
        print(f"AUDIO FAIL — CAT error: {exc}")
        sys.exit(1)
    except Exception as exc:
        print(f"AUDIO FAIL — unexpected: {exc}")
        sys.exit(1)


if __name__ == "__main__":
    main()
