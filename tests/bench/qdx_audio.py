"""tests/bench/qdx_audio.py — Reusable QDX USB Audio discovery + playback helpers.

Provides functions for finding the QDX's ALSA device, generating test tones,
and playing/capturing audio via aplay/arecord/sox.  Used by rung 2b and every
later TX-touching rung in the HWIL bench ladder.

Hardware notes (from live enumeration):
  - ALSA card name: "QDX Transceiver"
  - Format: S24_3LE stereo, 48000 Hz (both directions)
  - USB ID: 0483:a34c (STMicroelectronics)
"""

import os
import subprocess


class QdxAudioError(Exception):
    """Any audio-path error (device not found, aplay/arecord failure, etc.)."""


def find_qdx_card() -> int:
    """Scan /proc/asound/cards for 'QDX Transceiver', return card number.

    Raises QdxAudioError if no QDX audio device is found.
    """
    try:
        with open("/proc/asound/cards") as f:
            lines = f.readlines()
    except OSError as exc:
        raise QdxAudioError(f"cannot read /proc/asound/cards: {exc}") from exc

    for line in lines:
        if "QDX" in line:
            # Format: " 1 [QDX            ]: USB-Audio - QDX Transceiver"
            # Card number is the first non-space token.
            stripped = line.lstrip()
            parts = stripped.split(None, 1)
            if parts:
                try:
                    return int(parts[0])
                except ValueError:
                    pass

    raise QdxAudioError(
        "QDX Transceiver not found in /proc/asound/cards — "
        "is the QDX connected via USB?"
    )


def qdx_hw_device(card: int) -> str:
    """Return the ALSA hardware device string for the given card number."""
    return f"hw:{card},0"


def generate_tone(path: str, freq_hz: int = 1500, duration_s: float = 2.0,
                  sample_rate: int = 48000) -> None:
    """Use sox to generate a sine tone WAV in the QDX's native format.

    Generates S24 stereo 48 kHz — playable directly via hw: with no
    silent format conversion.  ALSA maps S24 (4-byte padded) WAV to
    the QDX's S24_3LE (3-byte packed) on hw: devices.

    Raises QdxAudioError on failure.
    """
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    cmd = [
        "sox", "-n", "-r", str(sample_rate), "-b", "24", "-c", "2",
        path, "synth", str(duration_s), "sine", str(freq_hz),
    ]
    try:
        subprocess.run(cmd, check=True, capture_output=True, timeout=10)
    except FileNotFoundError:
        raise QdxAudioError("sox not found — install sox")
    except subprocess.CalledProcessError as exc:
        raise QdxAudioError(
            f"sox tone generation failed (exit {exc.returncode}): "
            f"{exc.stderr.decode(errors='replace').strip()}"
        )


def play_to_qdx(wav_path: str, hw_device: str, timeout: int = 10) -> bool:
    """Play a WAV file to the QDX via aplay. Returns True on success.

    Uses hw: directly — the WAV must be in a format the QDX accepts
    (S24 stereo 48 kHz).  Format mismatches fail loud instead of being
    silently converted by plughw:.

    Raises QdxAudioError on failure.
    """
    cmd = ["aplay", "-D", hw_device, wav_path]
    try:
        subprocess.run(cmd, check=True, capture_output=True, timeout=timeout)
    except FileNotFoundError:
        raise QdxAudioError("aplay not found — install alsa-utils")
    except subprocess.TimeoutExpired:
        raise QdxAudioError(f"aplay timed out after {timeout}s")
    except subprocess.CalledProcessError as exc:
        raise QdxAudioError(
            f"aplay failed (exit {exc.returncode}): "
            f"{exc.stderr.decode(errors='replace').strip()}"
        )
    return True


def capture_from_qdx(wav_path: str, hw_device: str, duration_s: float = 2.0,
                     timeout: int = 10) -> bool:
    """Capture audio from the QDX via arecord. Returns True on success.

    Records in the QDX's native format: S24_3LE stereo, 48000 Hz.

    Raises QdxAudioError on failure.
    """
    os.makedirs(os.path.dirname(wav_path) or ".", exist_ok=True)
    cmd = [
        "arecord", "-D", hw_device,
        "-f", "S24_3LE", "-c", "2", "-r", "48000",
        "-d", str(int(duration_s)),
        wav_path,
    ]
    try:
        subprocess.run(cmd, check=True, capture_output=True, timeout=timeout)
    except FileNotFoundError:
        raise QdxAudioError("arecord not found — install alsa-utils")
    except subprocess.TimeoutExpired:
        raise QdxAudioError(f"arecord timed out after {timeout}s")
    except subprocess.CalledProcessError as exc:
        raise QdxAudioError(
            f"arecord failed (exit {exc.returncode}): "
            f"{exc.stderr.decode(errors='replace').strip()}"
        )
    return True
