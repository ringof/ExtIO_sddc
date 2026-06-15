#!/usr/bin/env python3
"""tests/bench/rung3_loopback_test.py — Rung 3: End-to-end RF loopback test.

QDX transmits a known tone at a known dial frequency; the attenuated RF
is received by the RX888 via ka9q-radio.  The script uses `powers` to
capture the spectrum and validates that the tone appears at the expected
carrier frequency above the noise floor.

This is the first fully closed-loop automated RF test in the bench ladder.

Prerequisites:
  - Container running with radiod streaming (RX888 capturing)
  - QDX connected via USB (CAT + audio)
  - QDX TX output coupled to RX888 input via attenuated cable
"""

import argparse
import csv
import io
import os
import shutil
import subprocess
import sys
import tempfile

# Allow importing qdx_cat / qdx_audio from the same directory.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from qdx_cat import QdxCat, QdxCatError
from qdx_audio import (
    QdxAudioError, find_qdx_card, qdx_hw_device, generate_tone,
)

# Output directory for generated files.
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")

# Defaults
DEFAULT_DIAL_HZ = 14_095_600   # 20m WSPR dial
DEFAULT_TONE_HZ = 1_500        # audio tone
TONE_DURATION_S = 10           # long enough for powers integration + margin
POWERS_INTEGRATION_S = 3
POWERS_BINS = 256
POWERS_BINWIDTH_HZ = 100       # 256 bins * 100 Hz = 25.6 kHz span
DEFAULT_THRESHOLD_DB = 10      # peak must be this many dB above median
DEFAULT_FREQ_TOL_HZ = 500      # peak must be within ±this of expected carrier
DEFAULT_SSRC = 30303
DEFAULT_GROUP = "hf.local,lo"


def run_powers(freq_hz: int, group: str, ssrc: int,
               integration_s: int = POWERS_INTEGRATION_S,
               bins: int = POWERS_BINS,
               binwidth: int = POWERS_BINWIDTH_HZ,
               timeout_s: int = 18) -> str:
    """Run `powers` and return its CSV stdout.

    Raises RuntimeError on failure.
    """
    cmd = [
        "powers",
        "-c", "1",                   # one snapshot
        "-i", str(integration_s),    # integration time
        "-f", str(freq_hz),          # center frequency
        "-b", str(bins),             # number of bins
        "-w", str(binwidth),         # bin width Hz
        "-s", str(ssrc),            # SSRC for temp channel
        group,
    ]
    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout_s,
        )
    except FileNotFoundError:
        raise RuntimeError("powers binary not found — is ka9q-radio installed?")
    except subprocess.TimeoutExpired:
        raise RuntimeError(f"powers timed out after {timeout_s}s")

    if result.returncode != 0:
        raise RuntimeError(
            f"powers failed (exit {result.returncode}): "
            f"{result.stderr.strip()}"
        )

    return result.stdout


def parse_powers_csv(csv_text: str) -> list[tuple[float, float]]:
    """Parse powers output into [(freq_hz, power_dbm), ...].

    powers outputs one line per snapshot:
        timestamp, low_freq, high_freq, binwidth, num_bins, p0, p1, p2, ...
    Bin frequencies are implicit: bin[i] = low_freq + i * binwidth.
    """
    bins = []
    for line in csv_text.strip().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = [p.strip() for p in line.split(",")]
        # Need at least: timestamp, low, high, binwidth, nbins, + 1 power
        if len(parts) < 6:
            continue
        try:
            low_freq = float(parts[1])
            binwidth = float(parts[3])
            powers = [float(p) for p in parts[5:]]
        except ValueError:
            continue
        for i, pwr in enumerate(powers):
            freq = low_freq + i * binwidth
            bins.append((freq, pwr))
        break  # use first valid data line only

    return bins


def median(values: list[float]) -> float:
    """Return the median of a list of floats."""
    s = sorted(values)
    n = len(s)
    if n == 0:
        return 0.0
    mid = n // 2
    if n % 2 == 0:
        return (s[mid - 1] + s[mid]) / 2.0
    return s[mid]


def main():
    p = argparse.ArgumentParser(
        description="Rung 3: End-to-end RF loopback test"
    )
    p.add_argument("--port", default="/dev/ttyACM0", help="QDX serial port")
    p.add_argument("--baud", type=int, default=9600, help="baud rate")
    p.add_argument("--freq", type=int, default=DEFAULT_DIAL_HZ,
                   help=f"dial frequency in Hz (default: {DEFAULT_DIAL_HZ})")
    p.add_argument("--tone", type=int, default=DEFAULT_TONE_HZ,
                   help=f"audio tone in Hz (default: {DEFAULT_TONE_HZ})")
    p.add_argument("--group", default=DEFAULT_GROUP,
                   help=f"powers status group (default: {DEFAULT_GROUP})")
    p.add_argument("--ssrc", type=int, default=DEFAULT_SSRC,
                   help=f"powers SSRC (default: {DEFAULT_SSRC})")
    p.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD_DB,
                   help=f"dB above median for pass (default: {DEFAULT_THRESHOLD_DB})")
    p.add_argument("--card", type=int, default=None,
                   help="ALSA card number (default: auto-discover)")
    args = p.parse_args()

    dial_hz = args.freq
    tone_hz = args.tone
    expected_carrier_hz = dial_hz + tone_hz

    os.makedirs(OUT_DIR, exist_ok=True)
    checks = 0

    try:
        # --- Preflight -------------------------------------------------------

        # powers binary exists?
        if not shutil.which("powers"):
            print("RUNG3 LOOPBACK FAIL — powers binary not found")
            sys.exit(1)

        # QDX audio device
        if args.card is not None:
            card = args.card
        else:
            card = find_qdx_card()
        hw = qdx_hw_device(card)

        with QdxCat(args.port, baudrate=args.baud) as qdx:
            # QDX alive?
            orig_freq = qdx.get_freq()
            print(f"RUNG3: QDX alive at {orig_freq} Hz")

            # --- Set dial frequency ------------------------------------------
            readback = qdx.set_freq(dial_hz)
            if readback != dial_hz:
                print(f"RUNG3 LOOPBACK FAIL — freq set: expected {dial_hz}, "
                      f"read back {readback}")
                sys.exit(1)
            print(f"RUNG3: set dial -> {dial_hz} Hz")

            # --- Generate tone WAV -------------------------------------------
            tone_path = os.path.join(OUT_DIR, "rung3_tone.wav")
            generate_tone(tone_path, freq_hz=tone_hz, duration_s=TONE_DURATION_S)

            # --- TX + capture (concurrent) -----------------------------------
            print(f"RUNG3: TX + {tone_hz} Hz tone ({TONE_DURATION_S} s)")

            qdx.tx_on()
            tx_state = qdx.get_tx_state()
            if not tx_state:
                print("RUNG3 LOOPBACK FAIL — TQ not TX after TX;")
                sys.exit(1)

            # Start aplay in background
            aplay_proc = subprocess.Popen(
                ["aplay", "-D", hw, tone_path],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
            )

            # Run powers in foreground (captures spectrum while tone plays).
            # First invocation after container start sometimes returns garbage;
            # retry once if no bins are parsed.
            powers_csv = None
            for attempt in range(2):
                try:
                    powers_csv = run_powers(
                        freq_hz=expected_carrier_hz,
                        group=args.group,
                        ssrc=args.ssrc,
                    )
                except RuntimeError as exc:
                    if attempt == 0:
                        print(f"RUNG3: powers attempt 1 failed ({exc}), retrying")
                        continue
                    # Second attempt failed — bail
                    aplay_proc.terminate()
                    aplay_proc.wait(timeout=5)
                    qdx.tx_off()
                    print(f"RUNG3 LOOPBACK FAIL — powers: {exc}")
                    sys.exit(1)

                if parse_powers_csv(powers_csv):
                    break  # got usable data
                if attempt == 0:
                    print("RUNG3: powers attempt 1 returned no bins, retrying")

            # Wait for aplay to finish
            try:
                aplay_proc.wait(timeout=TONE_DURATION_S + 5)
            except subprocess.TimeoutExpired:
                aplay_proc.terminate()
                aplay_proc.wait(timeout=5)

            qdx.tx_off()

            # --- Restore original frequency ----------------------------------
            qdx.set_freq(orig_freq)

        # --- Parse powers output ---------------------------------------------
        bins = parse_powers_csv(powers_csv)

        # --- Validate --------------------------------------------------------

        # Check 1: got spectrum
        if len(bins) == 0:
            print("RUNG3 LOOPBACK FAIL — powers returned no bins")
            sys.exit(1)
        checks += 1

        powers_list = [pwr for _, pwr in bins]
        peak_idx = max(range(len(bins)), key=lambda i: bins[i][1])
        peak_freq = bins[peak_idx][0]
        peak_pwr = bins[peak_idx][1]
        med_pwr = median(powers_list)

        print(f"RUNG3: powers capture -> {len(bins)} bins, "
              f"peak {peak_pwr:.1f} dB @ {peak_freq:.0f} Hz, "
              f"median {med_pwr:.1f} dB")

        # Check 2: peak within ±500 Hz of expected carrier
        freq_error = abs(peak_freq - expected_carrier_hz)
        if freq_error > DEFAULT_FREQ_TOL_HZ:
            print(f"RUNG3 LOOPBACK FAIL — peak @ {peak_freq:.0f} Hz, "
                  f"expected {expected_carrier_hz} Hz "
                  f"(error {freq_error:.0f} Hz > {DEFAULT_FREQ_TOL_HZ} Hz)")
            sys.exit(1)
        print(f"RUNG3: peak within +/-{DEFAULT_FREQ_TOL_HZ} Hz "
              f"of expected carrier OK")
        checks += 1

        # Check 3: peak above median by threshold
        margin = peak_pwr - med_pwr
        if margin < args.threshold:
            print(f"RUNG3 LOOPBACK FAIL — peak {margin:.1f} dB above median, "
                  f"need >= {args.threshold:.0f} dB")
            sys.exit(1)
        print(f"RUNG3: peak {margin:.1f} dB above median OK")
        checks += 1

        # --- Verdict ---------------------------------------------------------
        print(f"RUNG3 LOOPBACK OK ({checks} checks)")

    except QdxAudioError as exc:
        print(f"RUNG3 LOOPBACK FAIL — audio: {exc}")
        sys.exit(1)
    except QdxCatError as exc:
        print(f"RUNG3 LOOPBACK FAIL — CAT: {exc}")
        sys.exit(1)
    except Exception as exc:
        print(f"RUNG3 LOOPBACK FAIL — unexpected: {exc}")
        sys.exit(1)


if __name__ == "__main__":
    main()
