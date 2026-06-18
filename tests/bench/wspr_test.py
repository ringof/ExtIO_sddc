#!/usr/bin/env python3
"""tests/bench/wspr_test.py — WSPR decode test (80/40/30/20m).

QDX transmits a randomly-generated WSPR message on each of four bands.
The attenuated RF is received by the RX888 via ka9q-radio.  pcmrecord
captures slot-aligned WAV files from the [WSPR] channels, and wsprd
asserts the known message was decoded.

WSPR uses 2-minute slots (vs FT8's 15s), so 4 bands takes ~10 minutes
(plus slot alignment waits).

Prerequisites:
  - Container running with radiod streaming (RX888 capturing)
  - QDX connected via USB (CAT + audio)
  - QDX TX output coupled to RX888 input via attenuated cable
  - wsprsimwav, wsprd, sox, pcmrecord installed in the image
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
    {"name": "80m", "dial_hz": 3_568_600},
    {"name": "40m", "dial_hz": 7_038_600},
    {"name": "30m", "dial_hz": 10_138_700},
    {"name": "20m", "dial_hz": 14_095_600},
]

TMP_DIR = "/tmp/bench_wspr"
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
# WSPR slot timing
# ---------------------------------------------------------------------------

def wait_for_wspr_slot():
    """Sleep until the next WSPR slot boundary (even-minute :00).

    WSPR slots are 120 s (2 minutes), aligned to even UTC minutes.
    If we're within 5 s of a boundary we skip to the next one to avoid
    a race with audio playback startup.
    """
    now = time.time()
    elapsed = now % 120.0
    remaining = 120.0 - elapsed
    if remaining < 5.0:
        remaining += 120.0
    boundary = now + remaining
    print(f"WSPR: waiting {remaining:.1f}s for next WSPR slot boundary")
    time.sleep(remaining)
    return boundary


# ---------------------------------------------------------------------------
# Audio generation
# ---------------------------------------------------------------------------

def generate_wspr_wav(message, tmp_dir, drive_db=-1):
    """Generate WSPR TX audio (S24 stereo 48 kHz) for the QDX.

    wsprsimwav produces 48 kHz S16_LE mono.  sox converts to stereo S24
    for the QDX hw: device, normalized to *drive_db* dB.  The QDX needs
    near-full-scale audio to modulate; wsprsimwav's default level is too
    quiet.

    Returns the path to the 48 kHz stereo WAV file.
    """
    wav_48k_mono = os.path.join(tmp_dir, "wspr_48k_mono.wav")
    wav_48k_stereo = os.path.join(tmp_dir, "wspr_tx.wav")

    # wsprsimwav returns exit code 1 on success (upstream bug in main()).
    # Validate by output file existence, not exit code.
    result = subprocess.run(
        ["wsprsimwav", "-a", "-6", "-o", wav_48k_mono, message],
        capture_output=True, timeout=30,
    )
    if not os.path.isfile(wav_48k_mono) or os.path.getsize(wav_48k_mono) == 0:
        raise RuntimeError(
            f"wsprsimwav produced no output (exit {result.returncode}): "
            f"{result.stderr.decode(errors='replace').strip()}")

    # Convert mono S16 -> stereo S24 for QDX hw: device, normalized to
    # drive_db so the QDX sees enough level to modulate.
    subprocess.run(
        ["sox", wav_48k_mono, "-c", "2", "-b", "24", wav_48k_stereo,
         "gain", "-n", str(drive_db)],
        check=True, capture_output=True, timeout=10,
    )

    return wav_48k_stereo


# ---------------------------------------------------------------------------
# pcmrecord management
# ---------------------------------------------------------------------------

def start_pcmrecord(band_name, ssrc, group, caps_dir):
    """Start pcmrecord in the background for WSPR capture.

    Uses -w flag for WSPR mode (120s slot-aligned captures).
    Returns (subprocess.Popen, output_dir).
    """
    out_dir = os.path.join(caps_dir, band_name)
    os.makedirs(out_dir, exist_ok=True)

    cmd = [
        "pcmrecord",
        "-w",                    # WSPR mode: 120s slot-aligned captures
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

def decode_and_check(wav_path, expected_callsign, expected_grid,
                     expected_dbm, dial_mhz):
    """Run wsprd on wav_path, return (decoded: bool, snr: float|None).

    pcmrecord writes WAV with WAVE_FORMAT_EXTENSIBLE headers that wsprd
    may not parse.  Normalize through sox to 12 kHz mono S16 first.

    wsprd stdout format per decoded signal:
        HHMM  SNR  DT  FREQ  DRIFT  CALL GRID DBM
    e.g. "0000  -2  0.3   7.038647  0  WA3KRT JO47 30"
    """
    norm_path = wav_path + ".norm.wav"
    try:
        subprocess.run(
            ["sox", wav_path, "-r", "12000", "-c", "1", "-b", "16",
             "-e", "signed-integer", norm_path],
            check=True, capture_output=True, timeout=10,
        )
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
        print(f"WSPR:   sox normalize failed: {exc}")
        return False, None

    # wsprd writes scratch files (hashtable.txt, ALL_WSPR.TXT, etc.) into
    # its cwd, so use a dedicated work directory.
    work_dir = os.path.join(TMP_DIR, "wsprd_work")
    os.makedirs(work_dir, exist_ok=True)

    try:
        result = subprocess.run(
            ["wsprd", "-f", str(dial_mhz), norm_path],
            capture_output=True, text=True, timeout=120,
            cwd=work_dir,
        )
    except subprocess.TimeoutExpired:
        print(f"WSPR:   wsprd timed out on {wav_path}")
        return False, None

    print(f"WSPR:   wsprd exit={result.returncode}, "
          f"{len(result.stdout.splitlines())} line(s)")

    decoded = False
    snr = None

    for line in result.stdout.splitlines():
        line = line.strip()
        if not line or line.startswith("<"):
            continue
        print(f"WSPR:     {line}")

        # Match the expected callsign in wsprd output.
        # wsprd format: "HHMM  SNR  DT  FREQ  DRIFT  CALL GRID DBM"
        if expected_callsign in line:
            decoded = True
            # Parse SNR from the line (second field)
            parts = line.split()
            if len(parts) >= 2:
                try:
                    snr = float(parts[1])
                except ValueError:
                    pass

    return decoded, snr


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    p = argparse.ArgumentParser(
        description="WSPR decode test (80/40/30/20m)"
    )
    p.add_argument("--port", default="/dev/ttyACM0", help="QDX serial port")
    p.add_argument("--baud", type=int, default=9600, help="baud rate")
    p.add_argument("--card", type=int, default=None,
                   help="ALSA card number (default: auto-discover)")
    p.add_argument("--group", default="wspr-pcm.local",
                   help="pcmrecord data group (default: wspr-pcm.local)")
    p.add_argument("--message", default=None,
                   help="override auto-generated WSPR message (CALL GRID DBM)")
    p.add_argument("--drive", type=float, default=-1,
                   help="TX audio normalize level in dB (default: -1)")
    args = p.parse_args()

    # Preflight checks
    for tool in ("wsprsimwav", "wsprd", "sox", "pcmrecord", "aplay"):
        if not shutil.which(tool):
            print(f"WSPR FAIL — {tool} not found in PATH")
            sys.exit(1)

    # QDX audio device
    if args.card is not None:
        card = args.card
    else:
        card = find_qdx_card()
    hw = qdx_hw_device(card)
    print(f"WSPR: QDX audio device = {hw}")

    # Prepare temp dirs
    os.makedirs(TMP_DIR, exist_ok=True)
    os.makedirs(CAPS_DIR, exist_ok=True)

    passed = []
    failed = []
    pcmrec_proc = None

    try:
        with QdxCat(args.port, baudrate=args.baud) as qdx:
            orig_freq = qdx.get_freq()
            print(f"WSPR: QDX alive, original freq = {orig_freq} Hz")

            for band in BANDS:
                name = band["name"]
                dial_hz = band["dial_hz"]
                dial_mhz = dial_hz / 1_000_000.0
                ssrc = round(dial_hz / 1000)

                # Generate unique message for this band
                if args.message:
                    message = args.message
                else:
                    call = random_callsign()
                    grid = random_grid()
                    dbm = str(random.choice([23, 27, 30, 33, 37]))
                    message = f"{call} {grid} {dbm}"

                msg_parts = message.split()
                if len(msg_parts) != 3:
                    print(f"WSPR FAIL — message must be 'CALL GRID DBM', "
                          f"got: {message!r}")
                    sys.exit(1)
                msg_call, msg_grid, msg_dbm = msg_parts

                print(f"\nWSPR: === {name} ({dial_hz} Hz, SSRC {ssrc}) ===")
                print(f"WSPR:   message = \"{message}\"")

                # Generate TX audio for this band
                tx_wav = generate_wspr_wav(
                    message, TMP_DIR, drive_db=args.drive)

                # Set QDX frequency
                readback = qdx.set_freq(dial_hz)
                if readback != dial_hz:
                    print(f"WSPR:   FAIL — freq set: expected {dial_hz}, "
                          f"got {readback}")
                    failed.append(name)
                    continue
                print(f"WSPR:   dial set -> {dial_hz} Hz")

                # Clean capture directory
                cap_dir = os.path.join(CAPS_DIR, name)
                if os.path.exists(cap_dir):
                    shutil.rmtree(cap_dir)

                # Start pcmrecord
                pcmrec_proc, cap_dir = start_pcmrecord(
                    name, ssrc, args.group, CAPS_DIR)
                print(f"WSPR:   pcmrecord started (pid {pcmrec_proc.pid})")

                # Wait for WSPR slot boundary (even minute)
                boundary = wait_for_wspr_slot()

                # TX: key QDX + play audio (~110s)
                qdx.tx_on()
                tx_state = qdx.get_tx_state()
                if not tx_state:
                    print(f"WSPR:   FAIL — QDX not in TX after TX;")
                    stop_pcmrecord(pcmrec_proc)
                    pcmrec_proc = None
                    failed.append(name)
                    continue

                print(f"WSPR:   TX on, playing WSPR audio (~110s)")
                aplay_proc = subprocess.Popen(
                    ["aplay", "-D", hw, tx_wav],
                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                )

                # Wait for aplay to finish (WSPR audio is ~112s)
                try:
                    aplay_proc.wait(timeout=130)
                except subprocess.TimeoutExpired:
                    aplay_proc.terminate()
                    aplay_proc.wait(timeout=5)
                    print(f"WSPR:   WARNING — aplay timed out")

                qdx.tx_off()
                print(f"WSPR:   TX off")

                # Wait for pcmrecord to close the slot file.
                # The slot started at `boundary`; pcmrecord closes at
                # boundary + 120s. Add 3s grace.
                now = time.time()
                wait_until = boundary + 120.0 + 3.0
                if wait_until > now:
                    remaining = wait_until - now
                    print(f"WSPR:   waiting {remaining:.1f}s for slot closure")
                    time.sleep(remaining)

                # Stop pcmrecord
                stop_pcmrecord(pcmrec_proc)
                pcmrec_proc = None

                # Find captured WAVs
                wavs = sorted(glob.glob(os.path.join(cap_dir, "*.wav")))
                if not wavs:
                    print(f"WSPR:   FAIL — no WAV captured in {cap_dir}")
                    failed.append(name)
                    continue
                print(f"WSPR:   {len(wavs)} capture(s): "
                      + " ".join(os.path.basename(w) for w in wavs))

                # Decode
                band_decoded = False
                best_snr = None
                for cap_wav in wavs:
                    ok, snr = decode_and_check(
                        cap_wav, msg_call, msg_grid, msg_dbm, dial_mhz)
                    if ok:
                        band_decoded = True
                        best_snr = snr
                        snr_str = (f"SNR={snr:.0f}"
                                   if snr is not None else "SNR=?")
                        print(f"WSPR:   {name} PASS — \"{message}\" decoded "
                              f"({snr_str})")
                        break

                if band_decoded:
                    passed.append(name)
                else:
                    print(f"WSPR:   {name} FAIL — \"{message}\" not decoded")
                    failed.append(name)

            # Restore original frequency
            qdx.set_freq(orig_freq)
            print(f"\nWSPR: restored QDX freq -> {orig_freq} Hz")

    except QdxAudioError as exc:
        print(f"WSPR FAIL — audio: {exc}")
        sys.exit(1)
    except QdxCatError as exc:
        print(f"WSPR FAIL — CAT: {exc}")
        sys.exit(1)
    except Exception as exc:
        print(f"WSPR FAIL — unexpected: {exc}")
        raise
    finally:
        if pcmrec_proc is not None:
            stop_pcmrecord(pcmrec_proc)

    # Verdict
    print()
    if not failed:
        band_list = " ".join(p for p in passed)
        print(f"WSPR OK ({len(passed)} bands: {band_list})")
        shutil.rmtree(TMP_DIR, ignore_errors=True)
    else:
        fail_list = " ".join(failed)
        print(f"WSPR FAIL — {fail_list}")
        print(f"(temp files kept in {TMP_DIR} for debugging)")
        sys.exit(1)


if __name__ == "__main__":
    main()
