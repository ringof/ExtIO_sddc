#!/usr/bin/env python3
"""
vhf_tune.py — RX888 mk2 VHF front-end controller, over EP0 vendor commands.

Enter VHF, tune the R828D to a frequency, hold, standby on exit. Runs alongside
a streamer (EP0 control vs EP1 bulk; the two_actor_open pattern, firmware #143).
Failures are fatal by default (override with --force); any error during bring-up
standbys.

Driver core (RX888 class, constants, R828D register sequences) lives in
rx888_vhf.py — shared with the interactive TUI (vhf_fm_radio.py). This file
is the CLI wrapper: argument parsing, base-GPIO resolution, hold loop.

PORTING NOTES (the non-obvious, bench-learned lessons):
  * R828D read/write bit order is ASYMMETRIC (Rafael "R820T2 Register
    Description"): writes go MSB-first (the chip stores the byte verbatim), but
    reads stream LSB-first, so a standard I2C master receives every read byte
    bit-reversed from the datasheet's logical numbering. So: write values
    verbatim, but bit-reverse each read byte (as librtlsdr does) to recover
    logical order. Proven on this hardware: writing init 0x13 to reg 0x06 reads
    back 0xC8, and reg 0x00 reads wire 0x69 = bit-reverse of the datasheet's
    logical 0x96. Only the R828D needs this — the Si5351 reads normally.
  * Masked R828D writes need a software register shadow: read-modify-write
    against the last value you wrote, not a chip read-back (this mirrors the
    firmware's r82xx priv->regs[]).
  * CLKB is Si5351 CLK2 off PLL-B; reset PLL-B ONLY (0x80). A global / PLL-A
    reset would glitch CLKA (the ADC sample clock) and kill the stream.
  * LO = RF + IF (4.57 MHz). The PLL reference is the CLKB frequency you
    programmed (pll_ref), not a fixed crystal.
  * PLL lock = reg 0x02 logical bit 6 (mask 0x40 on a bit-reversed read);
    on no-lock, raise VCO current (reg 0x12 [7:5]).
  * GETSTATS exposes gpio_state at bytes [26:30] only when the payload is >= 30
    bytes (added after release v0.1.0) — length-check before trusting it.
  * EP0 control needs no interface claim, so this coexists with a streamer.

Requires: pyusb  (pip install pyusb)
"""

import argparse, signal, sys, time
try:
    import usb.core
except ImportError:
    usb = None

from rx888_vhf import (
    RX888, TuneError, firmware_load,
    R828D_REF_HZ, IF_CARRIER,
)


# ════════════════════════════════════════════════════════════════════════
#  Host-tool CLI — not part of the portable driver.
# ════════════════════════════════════════════════════════════════════════
def parse_args():
    ap = argparse.ArgumentParser(description="RX888 mk2 VHF tune (host-side, EP0)")
    ap.add_argument("rf_hz", type=float, nargs="?", default=None,
                    help="VHF RF frequency to tune, Hz (omit only with --standby)")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=None,
                    help="HF GPIO control word (hex) to build on; default: read it "
                         "live from GETSTATS so the running app's GPIO is preserved")
    ap.add_argument("--ref", type=float, default=R828D_REF_HZ,
                    help=f"R828D reference via Si5351 CLKB, Hz (default {R828D_REF_HZ}); "
                         f"the PLL math uses this value too")
    ap.add_argument("--bias-tee", action="store_true", help="VHF-port DC bias")
    ap.add_argument("--persist", action="store_true",
                    help="leave the tune active and exit (no standby on exit)")
    ap.add_argument("--standby", action="store_true",
                    help="just standby (R828D off + CLKB off + HF) and exit")
    ap.add_argument("--force", action="store_true",
                    help="proceed despite CLKB-off / ID-mismatch / no-lock (debug)")
    ap.add_argument("--load", metavar="IMG",
                    help="load firmware if in bootloader (via `fx3_cmd load IMG`)")
    ap.add_argument("--fx3-cmd", default="fx3_cmd", help="fx3_cmd binary for --load")
    ap.add_argument("--adc", metavar="RATE_HZ", type=float,
                    help="run STARTADC at RATE_HZ first (ADC sample clock)")
    args = ap.parse_args()
    if not args.standby and args.rf_hz is None:
        ap.error("rf_hz is required unless --standby")
    return args


def resolve_base(rx, arg_base):
    """The GPIO word to build VHF on: an explicit --base, else the live word so
    we flip only VHF_EN and keep the running app's ADC/GPIO bits."""
    if arg_base is not None:
        print(f"base GPIO 0x{arg_base:05X} (given)")
        return arg_base
    base = rx.read_gpio_state()
    if base is None:
        sys.exit("this firmware's GETSTATS does not expose GPIO state (release "
                 "v0.1.0 / fw 2.3 and earlier). Pass --base <HF GPIO word, hex> — "
                 "without it a whole-word write would zero the app's GPIO.")
    print(f"base GPIO 0x{base:05X} (read live from GETSTATS)")
    return base


def main():
    args = parse_args()
    if usb is None:
        sys.exit("pyusb is required: pip install pyusb")

    if args.load:
        firmware_load(args.load, args.fx3_cmd)

    rx = RX888()
    rx.check_alive()
    base = resolve_base(rx, args.base)

    if args.standby:
        sys.exit(0 if rx.standby(base) else "standby cleanup incomplete")

    if args.adc:
        rx.start_adc(int(args.adc)); print(f"STARTADC = {args.adc/1e6:.3f} MHz")

    # Bring-up: any failure or USB/I2C error standbys, unless we persist.
    failed, leave_active, cleanup_ok = None, False, True
    try:
        print(f"enter VHF: GPIO 0x{rx.enter_vhf(base, args.bias_tee):05X}")
        rx.clkb_on(int(args.ref)); print(f"CLKB = {args.ref/1e6:.3f} MHz")
        if not rx.clkb_verify() and not args.force:
            raise TuneError("CLKB reads back as OFF (override with --force)")

        idv = rx.r828d_probe()
        if idv is None:
            raise TuneError("R828D not reachable over I2C")
        if idv != 0x96 and not args.force:
            raise TuneError(f"R828D ID 0x{idv:02X} != 0x96 (override with --force)")

        rx.r828d_init(); print("R828D init + 8 MHz filter (IF 4.57 MHz)")
        if not rx.r828d_set_freq(int(args.rf_hz)) and not args.force:
            raise TuneError("R828D PLL did not lock / out of range (override with --force)")
        print(f"tuned {args.rf_hz/1e6:.4f} MHz -> IF ~4.57 MHz; stream the IF")

        if args.persist:
            print("persist: leaving tune active"); leave_active = True
        else:
            print("holding tune — Ctrl-C to standby and exit")
            signal.signal(signal.SIGINT, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
            while True:
                time.sleep(1)
    except KeyboardInterrupt:
        pass                                  # normal hold exit -> standby below
    except (TuneError, usb.core.USBError) as e:
        failed = str(e)
    finally:
        if not leave_active:
            cleanup_ok = rx.standby(base)

    if failed:
        sys.exit(f"FAIL: {failed}")
    if not cleanup_ok:
        sys.exit("standby cleanup incomplete")


if __name__ == "__main__":
    main()
