#!/usr/bin/env python3
"""tests/bench/ft8_test.py — End-to-end FT8 decode test.

QDX transmits a randomly-generated FT8 message on each of four bands
(80/40/30/20m).  The attenuated RF is received by the RX888 via
ka9q-radio.  pcmrecord captures slot-aligned WAV files from the [FT8]
channels, and decode_ft8 asserts the known message was decoded.

Each band runs --passes passes (default 3) with a fresh random message
per pass.  All passes must decode for a band to pass.

Prerequisites:
  - Container running with radiod streaming (RX888 capturing)
  - QDX connected via USB (CAT + audio)
  - QDX TX output coupled to RX888 input via attenuated cable
  - gen_ft8, decode_ft8, sox, pcmrecord installed in the image
"""

import argparse
import glob
import os
import random
import shutil
import signal
import string
import subprocess
import sys
import time

# Allow importing qdx_cat / qdx_audio from the same directory.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from qdx_cat import QdxCat, QdxCatError
from qdx_audio import QdxAudioError, find_qdx_card, qdx_hw_device

BANDS = [
    {"name": "80m", "dial_hz": 3_573_000, "ssrc": 3573},
    {"name": "40m", "dial_hz": 7_074_000, "ssrc": 7074},
    {"name": "30m", "dial_hz": 10_136_000, "ssrc": 10136},
    {"name": "20m", "dial_hz": 14_074_000, "ssrc": 14074},
]

TMP_DIR = "/tmp/bench_ft8"
CAPS_DIR = os.path.join(TMP_DIR, "caps")


# ---------------------------------------------------------------------------
# Unique message generation
# ---------------------------------------------------------------------------

def random_grid():
    """Random Maidenhead grid square (e.g. 'JO31')."""
    return (random.choice(string.ascii_uppercase[:18])
            + random.choice(string.ascii_uppercase[:18])
            + str(random.randint(0, 9))
            + str(random.randint(0, 9)))


def random_callsign():
    """Random realistic US ham callsign (e.g. 'WA3KRT')."""
    prefix = random.choice(["W", "K", "N", "AA", "AK", "WA", "KB"])
    region = str(random.randint(0, 9))
    suffix = "".join(random.choices(string.ascii_uppercase,
                                    k=random.choice([1, 2, 3])))
    return f"{prefix}{region}{suffix}"


# ---------------------------------------------------------------------------
# FT8 slot timing
# ---------------------------------------------------------------------------

def wait_for_slot(period_s=15):
    """Sleep until the next FT8 slot boundary (:00/:15/:30/:45).

    If we're within 2 s of a boundary we skip to the next one to avoid
    a race with audio playback startup.
    """
    now = time.time()
    elapsed = now % period_s
    remaining = period_s - elapsed
    if remaining < 2.0:
        remaining += period_s
    boundary = now + remaining
    print(f"FT8: waiting {remaining:.1f}s for next FT8 slot boundary")
    time.sleep(remaining)
    return boundary


# ---------------------------------------------------------------------------
# Audio generation
# ---------------------------------------------------------------------------

def generate_ft8_wav(message, tone_freq, tmp_dir):
    """Generate FT8 TX audio (S24 stereo 48 kHz) for the QDX.

    Returns the path to the 48 kHz WAV file.
    """
    wav_12k = os.path.join(tmp_dir, "ft8_12k.wav")
    wav_48k = os.path.join(tmp_dir, "ft8_tx.wav")

    # gen_ft8 produces a 12 kHz mono WAV
    subprocess.run(
        ["gen_ft8", message, wav_12k, str(tone_freq)],
        check=True, capture_output=True, timeout=10,
    )

    # Upsample to 48 kHz stereo S24 for QDX hw: device
    subprocess.run(
        ["sox", wav_12k, "-r", "48000", "-c", "2", "-b", "24", wav_48k],
        check=True, capture_output=True, timeout=10,
    )

    return wav_48k


# ---------------------------------------------------------------------------
# pcmrecord management
# ---------------------------------------------------------------------------

def start_pcmrecord(band_name, ssrc, group, caps_dir):
    """Start pcmrecord in the background for one band.

    Returns (subprocess.Popen, output_dir).
    """
    out_dir = os.path.join(caps_dir, band_name)
    os.makedirs(out_dir, exist_ok=True)

    cmd = [
        "pcmrecord",
        "-8",                    # FT8 mode: 15s slot-aligned captures
        "--ssrc", str(ssrc),     # select only this band's channel
        "-d", out_dir,           # output directory
        group,                   # data group
    ]
    proc = subprocess.Popen(
        cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
    )
    return proc, out_dir


def stop_pcmrecord(proc, timeout=5):
    """Gracefully stop a pcmrecord process."""
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=3)


# ---------------------------------------------------------------------------
# Decode
# ---------------------------------------------------------------------------

def decode_and_check(wav_path, expected_message):
    """Run decode_ft8 on wav_path, return True if expected_message found.

    pcmrecord writes WAV with WAVE_FORMAT_EXTENSIBLE headers that
    ft8_lib's simple parser cannot read.  Normalize through sox first.
    """
    norm_path = wav_path + ".norm.wav"
    try:
        subprocess.run(
            ["sox", wav_path, norm_path],
            check=True, capture_output=True, timeout=10,
        )
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
        print(f"FT8:   sox normalize failed: {exc}")
        return False

    try:
        result = subprocess.run(
            ["decode_ft8", norm_path],
            capture_output=True, text=True, timeout=30,
        )
    except subprocess.TimeoutExpired:
        print(f"FT8:   decode_ft8 timed out on {wav_path}")
        return False

    print(f"FT8:   decode_ft8 exit={result.returncode}, "
          f"{len(result.stdout.splitlines())} line(s)")
    for line in result.stdout.splitlines():
        print(f"FT8:     {line}")

    return expected_message in result.stdout


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    p = argparse.ArgumentParser(
        description="End-to-end FT8 decode test (4 bands)"
    )
    p.add_argument("--port", default="/dev/ttyACM0", help="QDX serial port")
    p.add_argument("--baud", type=int, default=9600, help="baud rate")
    p.add_argument("--card", type=int, default=None,
                   help="ALSA card number (default: auto-discover)")
    p.add_argument("--group", default="ft8-pcm.local",
                   help="pcmrecord data group (default: ft8-pcm.local)")
    p.add_argument("--message", default=None,
                   help="override auto-generated FT8 message")
    p.add_argument("--tone-freq", type=int, default=1500,
                   help="audio frequency in Hz for gen_ft8 (default: 1500)")
    p.add_argument("--passes", type=int, default=3,
                   help="passes per band; all must decode (default: 3)")
    args = p.parse_args()

    # Preflight checks
    for tool in ("gen_ft8", "decode_ft8", "sox", "pcmrecord", "aplay"):
        if not shutil.which(tool):
            print(f"FT8 FAIL — {tool} not found in PATH")
            sys.exit(1)

    # QDX audio device
    if args.card is not None:
        card = args.card
    else:
        card = find_qdx_card()
    hw = qdx_hw_device(card)
    print(f"FT8: QDX audio device = {hw}")

    # Prepare temp dirs
    os.makedirs(TMP_DIR, exist_ok=True)
    os.makedirs(CAPS_DIR, exist_ok=True)

    passed = []
    failed = []
    pcmrec_proc = None

    try:
        with QdxCat(args.port, baudrate=args.baud) as qdx:
            orig_freq = qdx.get_freq()
            print(f"FT8: QDX alive, original freq = {orig_freq} Hz")

            for band in BANDS:
                name = band["name"]
                dial_hz = band["dial_hz"]
                ssrc = band["ssrc"]

                band_passes = 0
                for pass_num in range(1, args.passes + 1):
                    # Fresh random message each pass (unless overridden)
                    message = (args.message
                               or f"CQ {random_callsign()} {random_grid()}")

                    print(f"\nFT8: === {name} pass {pass_num}/{args.passes} "
                          f"({dial_hz} Hz) ===")
                    print(f"FT8:   message = \"{message}\"")

                    # Generate TX audio for this pass
                    tx_wav = generate_ft8_wav(
                        message, args.tone_freq, TMP_DIR)

                    # Clean capture directory so stale files from prior
                    # passes don't interfere with file selection.
                    cap_dir = os.path.join(CAPS_DIR, name)
                    if os.path.exists(cap_dir):
                        shutil.rmtree(cap_dir)

                    # Set QDX frequency
                    readback = qdx.set_freq(dial_hz)
                    if readback != dial_hz:
                        print(f"FT8:   FAIL — freq set: expected {dial_hz}, "
                              f"got {readback}")
                        break
                    print(f"FT8:   dial set -> {dial_hz} Hz")

                    # Start pcmrecord
                    pcmrec_proc, cap_dir = start_pcmrecord(
                        name, ssrc, args.group, CAPS_DIR)
                    print(f"FT8:   pcmrecord started (pid {pcmrec_proc.pid})")

                    # Wait for FT8 slot boundary
                    boundary = wait_for_slot()

                    # TX: key QDX + play audio
                    qdx.tx_on()
                    tx_state = qdx.get_tx_state()
                    if not tx_state:
                        print(f"FT8:   FAIL — QDX not in TX after TX;")
                        stop_pcmrecord(pcmrec_proc)
                        pcmrec_proc = None
                        break

                    print(f"FT8:   TX on, playing FT8 audio")
                    aplay_proc = subprocess.Popen(
                        ["aplay", "-D", hw, tx_wav],
                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                    )

                    # Wait for aplay to finish (FT8 audio is ~13s)
                    try:
                        aplay_proc.wait(timeout=20)
                    except subprocess.TimeoutExpired:
                        aplay_proc.terminate()
                        aplay_proc.wait(timeout=5)
                        print(f"FT8:   WARNING — aplay timed out")

                    qdx.tx_off()
                    print(f"FT8:   TX off")

                    # Wait for pcmrecord to close the slot file.
                    # The slot started at `boundary`; pcmrecord closes at
                    # boundary + 15s. Add 2s grace.
                    now = time.time()
                    wait_until = boundary + 15.0 + 2.0
                    if wait_until > now:
                        time.sleep(wait_until - now)

                    # Stop pcmrecord
                    stop_pcmrecord(pcmrec_proc)
                    pcmrec_proc = None

                    # Find captured WAVs.  pcmrecord writes slot-aligned
                    # files; SIGTERM may leave a short partial for the next
                    # slot.  Try all captures — signal is in the full one.
                    wavs = sorted(glob.glob(os.path.join(cap_dir, "*.wav")))
                    if not wavs:
                        print(f"FT8:   FAIL — no WAV captured in {cap_dir}")
                        break
                    print(f"FT8:   {len(wavs)} capture(s): "
                          + " ".join(os.path.basename(w) for w in wavs))

                    decoded = False
                    for cap_wav in wavs:
                        if decode_and_check(cap_wav, message):
                            print(f"FT8:   pass {pass_num} PASS — "
                                  f"\"{message}\" decoded in "
                                  f"{os.path.basename(cap_wav)}")
                            decoded = True
                            break
                    if not decoded:
                        print(f"FT8:   pass {pass_num} FAIL — "
                              f"\"{message}\" not decoded")
                        break

                    band_passes += 1

                if band_passes == args.passes:
                    passed.append(name)
                else:
                    failed.append(f"{name}({band_passes}/{args.passes})")

            # Restore original frequency
            qdx.set_freq(orig_freq)
            print(f"\nFT8: restored QDX freq -> {orig_freq} Hz")

    except QdxAudioError as exc:
        print(f"FT8 FAIL — audio: {exc}")
        sys.exit(1)
    except QdxCatError as exc:
        print(f"FT8 FAIL — CAT: {exc}")
        sys.exit(1)
    except Exception as exc:
        print(f"FT8 FAIL — unexpected: {exc}")
        sys.exit(1)
    finally:
        # Kill any lingering pcmrecord
        if pcmrec_proc is not None:
            stop_pcmrecord(pcmrec_proc)

    # Verdict
    print()
    if not failed:
        band_list = " ".join(p for p in passed)
        print(f"FT8 OK ({len(passed)} bands x {args.passes} passes: "
              f"{band_list})")
        # Clean up temp on success
        shutil.rmtree(TMP_DIR, ignore_errors=True)
    else:
        fail_list = " ".join(failed)
        print(f"FT8 FAIL — {fail_list}")
        print(f"(temp files kept in {TMP_DIR} for debugging)")
        sys.exit(1)


if __name__ == "__main__":
    main()
