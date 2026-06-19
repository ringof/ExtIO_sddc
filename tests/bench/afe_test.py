#!/usr/bin/env python3
"""tests/bench/afe_test.py — RX888mk2 Analog Front-End bench test.

Sweeps the PE4304 attenuator, AD8370 VGA, and HF/VHF antenna switch while
measuring received power via ka9q-radio's `powers` tool.  Validates
monotonicity and range for each control.

Prerequisites:
  - RX888mk2 on USB with firmware loaded (PID 0x00F1)
  - ka9q-radio container running with radiod streaming
  - QDX connected via USB (CAT + audio)
  - QDX TX output coupled to RX888 input via attenuated cable
  - pyusb installed on the host (pip install pyusb)

Runs on the host (not inside the container).  Uses `docker exec` to invoke
`powers` inside the container, matching the loopback_test.py pattern.
"""

import argparse
import os
import subprocess
import sys
import time

# Allow importing from the same directory.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from qdx_cat import QdxCat, QdxCatError
from qdx_audio import (
    QdxAudioError, find_qdx_card, qdx_hw_device, generate_tone,
)
from rx888_afe import RX888AFE, RX888AFEError, VHF_EN
from loopback_test import parse_powers_csv, median

# ── Defaults ──────────────────────────────────────────────────────────
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
CONTAINER = os.environ.get("CONTAINER", "ka9q-radio")

DEFAULT_DIAL_HZ = 14_095_600   # 20m — same as loopback
DEFAULT_TONE_HZ = 1_500
TONE_DURATION_S = 180           # long tone for all three phases
POWERS_INTEGRATION_S = 2
POWERS_BINS = 256
POWERS_BINWIDTH_HZ = 100
DEFAULT_SSRC = 30304            # distinct from loopback's 30303
DEFAULT_GROUP = "hf.local,lo"

# ── Attenuator sweep parameters ──────────────────────────────────────
ATT_STEPS = [0, 8, 16, 24, 32, 40, 48, 56, 63]
ATT_MONO_TOL_DB = 1.5          # tolerance for monotonicity check
ATT_SPAN_MIN_DB = 25.0
ATT_SPAN_MAX_DB = 38.0

# ── VGA sweep parameters ─────────────────────────────────────────────
VGA_CODES = [0, 16, 32, 48, 64, 96, 127, 128, 160, 192, 224, 255]
VGA_MONO_TOL_DB = 1.5
VGA_SAFETY_DBM = -10.0          # bail if peak exceeds this

# ── HF/VHF switch parameters ─────────────────────────────────────────
SWITCH_DROP_MIN_DB = 15.0
SWITCH_RECOVERY_TOL_DB = 3.0
SWITCH_SETTLE_S = 0.5


def run_powers_docker(freq_hz, group, ssrc,
                      integration_s=POWERS_INTEGRATION_S,
                      bins=POWERS_BINS, binwidth=POWERS_BINWIDTH_HZ,
                      timeout_s=18):
    """Run `powers` inside the container via docker exec, return CSV stdout."""
    cmd = [
        "docker", "exec", CONTAINER, "powers",
        "-c", "1",
        "-i", str(integration_s),
        "-f", str(freq_hz),
        "-b", str(bins),
        "-w", str(binwidth),
        "-s", str(ssrc),
        group,
    ]
    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout_s,
        )
    except FileNotFoundError:
        raise RuntimeError("docker not found on PATH")
    except subprocess.TimeoutExpired:
        raise RuntimeError(f"powers timed out after {timeout_s}s")

    if result.returncode != 0:
        raise RuntimeError(
            f"powers failed (exit {result.returncode}): "
            f"{result.stderr.strip()}"
        )
    return result.stdout


def measure_peak(freq_hz, group, ssrc, label=""):
    """Take a powers measurement and return peak power in dBm.

    Retries once on empty result (first invocation after container start
    can return garbage — same pattern as loopback_test.py).
    """
    for attempt in range(2):
        try:
            csv_text = run_powers_docker(freq_hz, group, ssrc)
        except RuntimeError as exc:
            if attempt == 0:
                print(f"AFE: powers attempt 1 failed ({exc}), retrying")
                continue
            raise
        bins = parse_powers_csv(csv_text)
        if bins:
            powers = [p for _, p in bins]
            peak_idx = max(range(len(bins)), key=lambda i: bins[i][1])
            return bins[peak_idx][1]
        if attempt == 0:
            print(f"AFE: powers attempt 1 returned no bins{label}, retrying")
    raise RuntimeError(f"powers returned no usable data{label}")


def check_monotonic(values, direction, tolerance):
    """Check that *values* are monotonically increasing (direction='up')
    or decreasing (direction='down'), within *tolerance* dB.

    Returns (ok, violations) where violations is a list of (i, delta) pairs.
    """
    violations = []
    for i in range(1, len(values)):
        delta = values[i] - values[i - 1]
        if direction == "down" and delta > tolerance:
            violations.append((i, delta))
        elif direction == "up" and delta < -tolerance:
            violations.append((i, delta))
    return len(violations) == 0, violations


def main():
    p = argparse.ArgumentParser(description="RX888mk2 AFE bench test")
    p.add_argument("--port", default="/dev/ttyACM0", help="QDX serial port")
    p.add_argument("--baud", type=int, default=9600)
    p.add_argument("--freq", type=int, default=DEFAULT_DIAL_HZ,
                   help="dial frequency Hz")
    p.add_argument("--tone", type=int, default=DEFAULT_TONE_HZ,
                   help="audio tone Hz")
    p.add_argument("--group", default=DEFAULT_GROUP)
    p.add_argument("--ssrc", type=int, default=DEFAULT_SSRC)
    p.add_argument("--card", type=int, default=None,
                   help="ALSA card number (default: auto)")
    p.add_argument("--phase", default=None,
                   choices=["att", "vga", "switch"],
                   help="run only one phase (default: all three)")
    args = p.parse_args()

    run_att = args.phase in (None, "att")
    run_vga = args.phase in (None, "vga")
    run_switch = args.phase in (None, "switch")

    dial_hz = args.freq
    tone_hz = args.tone
    carrier_hz = dial_hz + tone_hz
    measurements = 0
    checks = 0

    os.makedirs(OUT_DIR, exist_ok=True)

    # ── Preflight ─────────────────────────────────────────────────────
    # Container alive?
    try:
        subprocess.run(
            ["docker", "exec", CONTAINER, "true"],
            capture_output=True, timeout=5,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired, subprocess.CalledProcessError):
        print(f"AFE FAIL — container '{CONTAINER}' not running")
        sys.exit(1)

    # RX888 on USB?
    try:
        afe = RX888AFE()
    except RX888AFEError as exc:
        print(f"AFE FAIL — {exc}")
        sys.exit(1)

    hw, fw_hi, fw_lo = afe.check_alive()
    print(f"AFE: RX888 alive (hwconfig=0x{hw:02X}, fw={fw_hi}.{fw_lo}), "
          f"container OK")
    if hw != 0x04:
        print(f"AFE WARN: hwconfig 0x{hw:02X} is not RX888r2 (0x04)")

    # Save initial GPIO for restore
    initial_gpio = afe.read_gpio()

    # QDX audio device
    if args.card is not None:
        card = args.card
    else:
        card = find_qdx_card()
    hw_dev = qdx_hw_device(card)

    aplay_proc = None

    try:
        with QdxCat(args.port, baudrate=args.baud) as qdx:
            orig_freq = qdx.get_freq()

            # ── Setup: QDX TX continuous tone ─────────────────────────
            readback = qdx.set_freq(dial_hz)
            if readback != dial_hz:
                print(f"AFE FAIL — freq set: expected {dial_hz}, "
                      f"read back {readback}")
                sys.exit(1)

            tone_path = os.path.join(OUT_DIR, "afe_tone.wav")
            generate_tone(tone_path, freq_hz=tone_hz,
                          duration_s=TONE_DURATION_S)

            qdx.tx_on()
            if not qdx.get_tx_state():
                print("AFE FAIL — TQ not TX after TX;")
                sys.exit(1)

            aplay_proc = subprocess.Popen(
                ["aplay", "-D", hw_dev, tone_path],
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            )

            # Baseline with safe defaults
            afe.set_att(63)
            afe.set_vga(0)
            time.sleep(0.3)

            baseline = measure_peak(carrier_hz, args.group, args.ssrc)
            measurements += 1
            print(f"AFE: QDX alive at {dial_hz} Hz, "
                  f"baseline {baseline:.1f} dBm")

            # ══════════════════════════════════════════════════════════
            # Phase 1: Attenuator sweep (VGA=0 fixed)
            # ══════════════════════════════════════════════════════════
            if run_att:
                afe.set_vga(0)
                att_powers = []

                for step in ATT_STEPS:
                    cnt_before = afe.vendor_rqt_count()
                    afe.set_att(step)
                    cnt_after = afe.vendor_rqt_count()
                    time.sleep(0.2)
                    pwr = measure_peak(carrier_hz, args.group, args.ssrc)
                    att_powers.append(pwr)
                    measurements += 1
                    print(f"AFE ATT: step {step:2d} -> {pwr:.1f} dBm"
                          f"  [rqt {cnt_before}->{cnt_after}]")

                # Check: monotonically decreasing
                mono_ok, violations = check_monotonic(
                    att_powers, "down", ATT_MONO_TOL_DB)
                if not mono_ok:
                    for i, delta in violations:
                        print(f"AFE ATT FAIL — step {ATT_STEPS[i]}: "
                              f"+{delta:.1f} dB (should decrease)")
                    print("AFE FAIL — attenuator not monotonic")
                    sys.exit(1)
                checks += 1

                # Check: total span
                span = att_powers[0] - att_powers[-1]
                if not (ATT_SPAN_MIN_DB <= span <= ATT_SPAN_MAX_DB):
                    print(f"AFE FAIL — att span {span:.1f} dB, "
                          f"expected {ATT_SPAN_MIN_DB}–{ATT_SPAN_MAX_DB} dB")
                    sys.exit(1)
                checks += 1
                print(f"AFE ATT: span {span:.1f} dB, monotonic OK")

            # ══════════════════════════════════════════════════════════
            # Phase 2: VGA sweep (ATT=63 fixed)
            # ══════════════════════════════════════════════════════════
            if run_vga:
                afe.set_att(63)
                vga_powers = []
                safety_bail = False

                for code in VGA_CODES:
                    afe.set_vga(code)
                    time.sleep(0.2)
                    pwr = measure_peak(carrier_hz, args.group, args.ssrc)
                    vga_powers.append(pwr)
                    measurements += 1
                    print(f"AFE VGA: code {code:3d} -> {pwr:.1f} dBm"
                          f" (att=63)")

                    if pwr > VGA_SAFETY_DBM:
                        print(f"AFE VGA: SAFETY BAIL — peak {pwr:.1f} dBm "
                              f"> {VGA_SAFETY_DBM:.0f} dBm limit")
                        safety_bail = True
                        break

                # Restore safe state immediately
                afe.set_vga(0)
                afe.set_att(63)

                if safety_bail:
                    print("AFE FAIL — VGA safety limit exceeded")
                    sys.exit(1)

                # Split into low-gain (0–127) and high-gain (128–255) ranges
                low_codes = [c for c in VGA_CODES if c <= 127]
                high_codes = [c for c in VGA_CODES if c >= 128]
                low_powers = vga_powers[:len(low_codes)]
                high_powers = vga_powers[len(low_codes):]

                # Check: low-gain monotonically increasing
                low_ok, violations = check_monotonic(
                    low_powers, "up", VGA_MONO_TOL_DB)
                if not low_ok:
                    for i, delta in violations:
                        print(f"AFE VGA FAIL — low-gain code "
                              f"{low_codes[i]}: {delta:.1f} dB "
                              f"(should increase)")
                    print("AFE FAIL — VGA low-gain not monotonic")
                    sys.exit(1)
                checks += 1

                # Check: high-gain monotonically increasing
                high_ok, violations = check_monotonic(
                    high_powers, "up", VGA_MONO_TOL_DB)
                if not high_ok:
                    for i, delta in violations:
                        print(f"AFE VGA FAIL — high-gain code "
                              f"{high_codes[i]}: {delta:.1f} dB "
                              f"(should increase)")
                    print("AFE FAIL — VGA high-gain not monotonic")
                    sys.exit(1)
                checks += 1
                print(f"AFE VGA: low-gain OK, high-gain OK")

            # ══════════════════════════════════════════════════════════
            # Phase 3: HF/VHF switch
            # ══════════════════════════════════════════════════════════
            if run_switch:
                afe.set_att(63)
                afe.set_vga(0)
                time.sleep(0.3)

                # HF baseline (VHF_EN=0)
                hf_baseline = measure_peak(carrier_hz, args.group, args.ssrc)
                measurements += 1
                print(f"AFE HF/VHF: HF baseline {hf_baseline:.1f} dBm")

                # Read current GPIO, set VHF_EN
                gpio_word = afe.read_gpio()
                if gpio_word is None:
                    print("AFE FAIL — cannot read GPIO state (fw too old?)")
                    sys.exit(1)

                afe.gpio(gpio_word | VHF_EN)
                time.sleep(SWITCH_SETTLE_S)

                vhf_pwr = measure_peak(carrier_hz, args.group, args.ssrc)
                measurements += 1
                drop = hf_baseline - vhf_pwr
                print(f"AFE HF/VHF: VHF_EN=1 -> {vhf_pwr:.1f} dBm "
                      f"(drop {drop:.1f} dB)")

                # Check: signal drops when switched to VHF
                if drop < SWITCH_DROP_MIN_DB:
                    print(f"AFE FAIL — VHF drop {drop:.1f} dB "
                          f"< {SWITCH_DROP_MIN_DB:.0f} dB minimum")
                    sys.exit(1)
                checks += 1

                # Clear VHF_EN, recover HF
                afe.gpio(gpio_word & ~VHF_EN)
                time.sleep(SWITCH_SETTLE_S)

                recovery_pwr = measure_peak(carrier_hz, args.group, args.ssrc)
                measurements += 1
                recovery_delta = abs(recovery_pwr - hf_baseline)
                print(f"AFE HF/VHF: VHF_EN=0 -> {recovery_pwr:.1f} dBm "
                      f"(recovered)")

                # Check: HF recovery within tolerance
                if recovery_delta > SWITCH_RECOVERY_TOL_DB:
                    print(f"AFE FAIL — HF recovery delta {recovery_delta:.1f} dB "
                          f"> {SWITCH_RECOVERY_TOL_DB:.0f} dB tolerance")
                    sys.exit(1)
                checks += 1

            # ══════════════════════════════════════════════════════════
            # Teardown
            # ══════════════════════════════════════════════════════════
            afe.set_att(63)
            afe.set_vga(0)
            if initial_gpio is not None:
                afe.gpio(initial_gpio)

            qdx.tx_off()
            qdx.set_freq(orig_freq)

        # Kill aplay if still running
        if aplay_proc and aplay_proc.poll() is None:
            aplay_proc.terminate()
            aplay_proc.wait(timeout=5)

        print(f"AFE OK ({checks} checks, {measurements} measurements)")

    except (QdxCatError, QdxAudioError) as exc:
        print(f"AFE FAIL — {exc}")
        sys.exit(1)
    except RX888AFEError as exc:
        print(f"AFE FAIL — RX888: {exc}")
        sys.exit(1)
    except Exception as exc:
        print(f"AFE FAIL — unexpected: {exc}")
        sys.exit(1)
    finally:
        # Best-effort cleanup regardless of exit path
        try:
            afe.set_att(63)
            afe.set_vga(0)
            if initial_gpio is not None:
                afe.gpio(initial_gpio)
        except Exception:
            pass
        if aplay_proc and aplay_proc.poll() is None:
            try:
                aplay_proc.terminate()
                aplay_proc.wait(timeout=5)
            except Exception:
                pass


if __name__ == "__main__":
    main()
