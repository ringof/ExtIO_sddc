#!/usr/bin/env python3
"""tests/bench/rf_test.py — Manual RF verification tool.

Generates a tone, keys PTT, and plays it into the QDX so the operator
can confirm RF output on a separate receiver or via ka9q powers.

This is a manual tool, not an automated test.  It transmits for
--duration seconds (default 30) to give time for external measurement.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from qdx_cat import QdxCat, QdxCatError
from qdx_audio import (
    QdxAudioError, find_qdx_card, qdx_hw_device,
    generate_tone, play_to_qdx,
)

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")


def main():
    p = argparse.ArgumentParser(description="Manual QDX RF verification")
    p.add_argument("--port", default="/dev/ttyACM0", help="QDX serial port")
    p.add_argument("--baud", type=int, default=9600, help="baud rate")
    p.add_argument("--freq", type=int, default=None,
                   help="dial frequency in Hz (default: read current)")
    p.add_argument("--tone", type=int, default=1500,
                   help="audio tone frequency in Hz (default: 1500)")
    p.add_argument("--duration", type=float, default=30,
                   help="tone duration in seconds (default: 30)")
    args = p.parse_args()

    os.makedirs(OUT_DIR, exist_ok=True)

    try:
        card = find_qdx_card()
        hw = qdx_hw_device(card)
        print(f"Audio: card {card}, {hw}")

        tone_path = os.path.join(OUT_DIR, "bench_tone.wav")
        generate_tone(tone_path, freq_hz=args.tone, duration_s=args.duration)
        print(f"Tone: {tone_path} ({args.tone} Hz, {args.duration} s, S24 stereo 48 kHz)")

        with QdxCat(args.port, baudrate=args.baud) as q:
            if args.freq is not None:
                actual = q.set_freq(args.freq)
                print(f"Freq: set to {actual} Hz")
            else:
                actual = q.get_freq()
                print(f"Freq: {actual} Hz (current, use --freq to change)")

            carrier = actual + args.tone
            print(f"Expected carrier: {carrier} Hz")
            input("Press Enter to key PTT and play tone...")

            q.tx_on()
            print(f"PTT ON — transmitting {args.duration}s...")
            play_to_qdx(tone_path, hw, timeout=int(args.duration) + 10)
            print("Playback done.")
            q.tx_off()
            print("PTT OFF.")

    except (QdxCatError, QdxAudioError) as exc:
        print(f"BENCH RF ERROR — {exc}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nInterrupted.")
        sys.exit(1)


if __name__ == "__main__":
    main()
