#!/usr/bin/env python3
"""
vhf_tune.py — RX888 mk2 VHF front-end: enter+init, tune to a frequency, and
standby on exit. All over EP0 vendor control transfers; runs concurrently with
a streamer (EP0 vs EP1, the two_actor_open pattern, firmware test #143).

Lifecycle:
  enter   : select VHF (GPIOFX3 VHF_EN) + Si5351 CLKB + R828D init
  tune    : R828D set_freq to the LO for the requested RF (lock-checked)
  hold    : keep the tune live (stream in another terminal) until Ctrl-C
  exit    : R828D standby + CLKB off + back to HF   (skip with --persist)

Failures are fatal by default: an unreachable tuner, an R828D ID mismatch, or a
PLL that won't lock aborts and standbys (override with --force). Any USB/I2C
error during bring-up also triggers standby cleanup.

The register sequences are ported from the original firmware
(si5351aSetFrequencyB, r82xx_init/set_bandwidth/set_mux/set_pll/standby, 0ffa512)
and the host radio class (RX888R2Radio.cpp: R828D_FREQ=16 MHz, IF=4.57 MHz).
This is a PARTIAL port of r82xx_set_freq64: it does set_mux + set_pll + the
Air-In/Cable1 input switch, but NOT the harmonic-retry path (only needed above
the VCO/mixer ceiling, ~>1.7 GHz). Ported, bench-validated for lock at 144 MHz;
treat the math as needing confirmation outside the initially tested band.

Requires: pyusb  (pip install pyusb)
"""

import argparse, struct, sys, time, signal, subprocess
try:
    import usb.core, usb.util
except ImportError:
    usb = None

RX888_VID, RX888_PID = 0x04B4, 0x00F1
RX888_PID_BOOT = 0x00F3

# EP0 vendor commands (protocol.h)
TESTFX3, GPIOFX3, I2CWFX3, I2CRFX3, STARTADC, GETSTATS = 0xAC, 0xAD, 0xAE, 0xAF, 0xB2, 0xB3
BM_OUT, BM_IN = 0x40, 0xC0

# GPIO bits (protocol.h enum GPIOPin)
BIAS_HF, BIAS_VHF, VHF_EN = 1 << 8, 1 << 9, 1 << 15

# I2C addresses (8-bit form the firmware's I2cTransfer uses)
R828D_ADDR, SI5351_ADDR = 0x74, 0xC0

# Confirmed board constants (RX888R2Radio.cpp)
R828D_REF_HZ = 16_000_000        # tuner reference (Si5351 CLKB)
IF_CARRIER   = 4_570_000         # IF center (8 MHz filter)
SI5351_XTAL  = 27_000_000        # Si5351 crystal (Si5351.c SI5351_FREQ)

# Si5351 register addresses
SI_PLL_B, SI_MS2, SI_PLL_RESET, SI_CLK2 = 34, 58, 177, 18

# R828D init array, regs 0x05..0x1f (r82xx_init_array, DEFAULT_IF_VGA_VAL=11,
# VER_NUM=49 resolved)
R828D_INIT = [0x80,0x13,0x70,0xC0,0x40,0xDB,0x6B,0xEB,0x53,0x75,0x68,0x6C,0xBB,
              0x80,0x31,0x0F,0x00,0xC0,0x30,0x48,0xEC,0x60,0x00,0x24,0xDD,0x0E,0x40]
R828D_INIT_BASE = 0x05

# Tracking-filter bands: (start_MHz, open_d, rf_mux_ploy, tf_c). xtal_cap0p=0.
FREQ_RANGES = [
    (0,0x08,0x02,0xDF),(50,0x08,0x02,0xBE),(55,0x08,0x02,0x8B),(60,0x08,0x02,0x7B),
    (65,0x08,0x02,0x69),(70,0x08,0x02,0x58),(75,0x00,0x02,0x44),(80,0x00,0x02,0x44),
    (90,0x00,0x02,0x34),(100,0x00,0x02,0x34),(110,0x00,0x02,0x24),(120,0x00,0x02,0x24),
    (140,0x00,0x02,0x14),(180,0x00,0x02,0x13),(220,0x00,0x02,0x13),(250,0x00,0x02,0x11),
    (280,0x00,0x02,0x00),(310,0x00,0x41,0x00),(450,0x00,0x41,0x00),(588,0x00,0x40,0x00),
    (650,0x00,0x40,0x00)]


class TuneError(Exception):
    """Bring-up failure (unreachable tuner, ID mismatch, no PLL lock, ...)."""


# NOTE: the FX3 I2CRFX3 path returns R828D registers in canonical order
# (reg 0x00 reads 0x69 directly on hardware — the same order librtlsdr uses
# *after* its per-byte bit-reverse), so NO host-side reversal is needed.


class RX888:
    def __init__(self):
        self.dev = usb.core.find(idVendor=RX888_VID, idProduct=RX888_PID)
        if self.dev is None:
            sys.exit("RX888 mk2 not found (04B4:00F1).")
        self.regs = {}                # R828D shadow (firmware-style; masked writes read this)
        self.ref_hz = R828D_REF_HZ    # active R828D reference (Si5351 CLKB); set by clkb_on

    # --- EP0 primitives ---
    def gpio(self, word):
        self.dev.ctrl_transfer(BM_OUT, GPIOFX3, 0, 0, struct.pack("<I", word))

    def start_adc(self, rate_hz):
        """STARTADC: set the ADC sample clock (Si5351 CLKA) and enable the ADC
        clock flag that STARTFX3 requires. 4-byte LE payload, like GPIOFX3."""
        self.dev.ctrl_transfer(BM_OUT, STARTADC, 0, 0, struct.pack("<I", rate_hz))

    def i2c_w(self, addr, reg, data):
        self.dev.ctrl_transfer(BM_OUT, I2CWFX3, addr, reg, bytes(data))

    def i2c_r(self, addr, reg, n):
        return bytes(self.dev.ctrl_transfer(BM_IN, I2CRFX3, addr, reg, n))

    # --- liveness / verification ---
    def check_alive(self):
        """Confirm the FIRMWARE answers vendor commands (beyond lsusb): TESTFX3
        returns [hwconfig, fw_major, fw_minor, vendor_rqt_count]. wValue=0 so we
        don't toggle debug mode."""
        try:
            info = bytes(self.dev.ctrl_transfer(BM_IN, TESTFX3, 0, 0, 4))
        except usb.core.USBError as e:
            sys.exit(f"RX888 is on USB but firmware is not responding to TESTFX3 "
                     f"({e}). Powered up? claimed by another process?")
        hw, fwhi, fwlo, rqt = info[0], info[1], info[2], info[3]
        print(f"alive: hwconfig=0x{hw:02X} fw={fwhi}.{fwlo} vendor_rqts={rqt}")
        if hw != 0x04:
            print(f"  WARNING: hwconfig 0x{hw:02X} is not RX888r2 (0x04)")
        return hw

    def read_gpio_state(self):
        """Current steady-state GPIO word from GETSTATS (bytes 26..29, packed
        with the GPIOFX3 bit positions). Lets us preserve whatever the running
        host app (e.g. ka9q-radio) set and flip only VHF_EN.

        Returns None if the firmware's GETSTATS payload predates the gpio_state
        field (< 30 bytes — e.g. release v0.1.0 / fw 2.3). The caller must then
        require an explicit --base rather than silently zero the GPIO."""
        buf = bytes(self.dev.ctrl_transfer(BM_IN, GETSTATS, 0, 0, 64))
        if len(buf) < 30:
            return None
        return int.from_bytes(buf[26:30], "little")

    def clkb_verify(self):
        """Read back Si5351 CLK2_CONTROL (reads are NOT bit-reversed) to confirm
        CLKB is actually enabled — verifies the Si5351 write path took."""
        v = self.i2c_r(SI5351_ADDR, SI_CLK2, 1)[0]
        on = not (v & 0x80)                       # bit7 = CLK2_PDN
        print(f"  CLKB verify: CLK2_CTRL=0x{v:02X} -> {'enabled' if on else 'OFF'}")
        return on

    def r828d_probe(self):
        """Read the R828D ID register (0x00). Returns the byte (0x69 expected),
        or None if the tuner doesn't ACK over I2C. The FX3 I2CRFX3 path returns
        canonical byte order, so no host-side bit-reverse."""
        try:
            v = self.i2c_r(R828D_ADDR, 0x00, 1)[0]
        except usb.core.USBError as e:
            print(f"  R828D probe: no I2C response ({e}) — tuner unreachable")
            return None
        print(f"  R828D probe: reg0=0x{v:02X} (want 0x69) -> "
              f"{'OK' if v == 0x69 else 'ID MISMATCH'}")
        return v

    # --- R828D register access (shadowed, like the firmware) ---
    def _wr(self, reg, val):
        self.regs[reg] = val & 0xFF
        self.i2c_w(R828D_ADDR, reg, [val & 0xFF])

    def _wr_mask(self, reg, val, mask):
        cur = self.regs.get(reg, 0)
        self._wr(reg, (cur & ~mask) | (val & mask))

    def _rd(self, reg, n):
        return list(self.i2c_r(R828D_ADDR, reg, n))   # canonical order; no reverse

    # --- (1) Si5351 CLKB = ref_hz on CLK2/PLL-B (port of si5351aSetFrequencyB) ---
    def clkb_on(self, ref_hz):
        self.ref_hz = ref_hz                          # remember it for set_pll (pll_ref)
        freq, rdiv = ref_hz, 0
        while freq <= 1_000_000:
            freq *= 2; rdiv += 0x10
        divider = 900_000_000 // freq
        if divider % 2: divider -= 1
        pll = divider * freq
        mult = pll // SI5351_XTAL
        num  = ((pll % SI5351_XTAL) * 1048575) // SI5351_XTAL
        denom = 1048575
        self.i2c_w(SI5351_ADDR, SI_PLL_B, self._si_regs(mult, num, denom))
        self.i2c_w(SI5351_ADDR, SI_MS2,   self._si_regs(divider, 0, 1, rdiv))
        self.i2c_w(SI5351_ADDR, SI_PLL_RESET, [0x80])           # reset PLL-B only
        self.i2c_w(SI5351_ADDR, SI_CLK2, [0x4C | 0x20])         # enable CLK2 from PLL-B

    def clkb_off(self):
        self.i2c_w(SI5351_ADDR, SI_CLK2, [0x80])

    @staticmethod
    def _si_regs(a, num, denom, rdiv=None):
        # PLL: a=mult. MS: a=divider, rdiv given. P1/P2/P3 encoding (SetupPLL/SetupMultisynth).
        if rdiv is None:        # PLL
            P1 = 128 * a + (128 * num) // denom - 512
            P2 = 128 * num - denom * ((128 * num) // denom)
            P3 = denom
            d2 = (P1 >> 16) & 0x03
        else:                   # multisynth (integer divider)
            P1 = 128 * a - 512; P2 = 0; P3 = 1
            d2 = ((P1 >> 16) & 0x03) | rdiv
        return [(P3 >> 8) & 0xFF, P3 & 0xFF, d2, (P1 >> 8) & 0xFF, P1 & 0xFF,
                ((P3 >> 12) & 0xF0) | ((P2 >> 16) & 0x0F), (P2 >> 8) & 0xFF, P2 & 0xFF]

    # --- (2) R828D init (port of r82xx_init + set_bandwidth(8 MHz)) ---
    def r828d_init(self):
        for i, v in enumerate(R828D_INIT):           # seed shadow + write 0x05..0x1f
            reg = R828D_INIT_BASE + i
            self.regs[reg] = v
            self.i2c_w(R828D_ADDR, reg, [v])
        # set_bandwidth(8 MHz): IF channel filter -> IF center 4.57 MHz
        self._wr_mask(0x0A, 0x10, 0x0F)
        self._wr_mask(0x0B, 0x0B, 0xEF)
        self._wr_mask(0x1E, 0x60, 0x40)

    # --- (3) R828D tune (partial port of set_freq64 -> set_mux + set_pll + input sw) ---
    def r828d_set_freq(self, rf_hz):
        """LO = RF + IF (low-side injection). Runs set_mux + set_pll, then the
        R828D Air-In/Cable1 input switch. No harmonic-retry path (out of VHF
        scope). Returns True iff the PLL locked."""
        lo = rf_hz + IF_CARRIER
        self._set_mux(lo)
        ok = self._set_pll(lo)
        # R828D input selection: Air-In below 345 MHz, Cable1 above (reg 0x05).
        self._wr_mask(0x05, 0x00 if rf_hz > 345_000_000 else 0x60, 0x60)
        return ok

    def _set_mux(self, lo):
        mhz = lo // 1_000_000
        band = FREQ_RANGES[0]
        for r in FREQ_RANGES:
            if mhz < r[0]: break
            band = r
        _, open_d, rf_mux_ploy, tf_c = band
        self._wr_mask(0x17, open_d, 0x08)
        self._wr_mask(0x1A, rf_mux_ploy, 0xC3)
        self._wr(0x1B, tf_c)
        self._wr_mask(0x10, 0x00 | 0x08, 0x0B)        # default xtal cap (0pF | drive)

    def _set_pll(self, lo):
        VCO_MIN, VCO_MAX = 1_770_000, 3_540_000       # kHz
        VCO_POWER_REF = 1                             # R828D
        freq_khz = (lo + 500) // 1000
        pll_ref = self.ref_hz                         # the reference we programmed on CLKB
        self._wr_mask(0x10, 0x00, 0x10)               # refdiv2 = 0
        self._wr_mask(0x1A, 0x00, 0x0C)               # PLL autotune 128 kHz
        self._wr_mask(0x12, 0x80, 0xE0)               # VCO current min (0x80)

        mix_div, div_num, found = 2, 0, False
        while mix_div <= 64:
            if VCO_MIN <= freq_khz * mix_div < VCO_MAX:
                found = True
                b = mix_div
                while b > 2:
                    b >>= 1; div_num += 1
                break
            mix_div <<= 1
        if not found:
            print(f"  set_pll: LO {lo/1e6:.4f} MHz out of range "
                  f"(needs VCO {VCO_MIN/1e3:.0f}-{VCO_MAX/1e3:.0f} MHz via mix_div 2..64)")
            return False

        data = self._rd(0x00, 5)
        vco_fine = (data[4] & 0x30) >> 4
        if vco_fine > VCO_POWER_REF: div_num -= 1
        elif vco_fine < VCO_POWER_REF: div_num += 1
        self._wr_mask(0x10, div_num << 5, 0xE0)

        vco_freq = lo * mix_div
        vco_div = (pll_ref + 65536 * vco_freq) // (2 * pll_ref)
        nint, sdm = vco_div // 65536, vco_div % 65536
        if nint > (128 // VCO_POWER_REF) - 1:
            print(f"  set_pll: no valid PLL for {lo/1e6:.4f} MHz LO "
                  f"at ref {pll_ref/1e6:.3f} MHz (nint={nint})")
            return False

        ni = (nint - 13) // 4
        si = nint - 4 * ni - 13
        self._wr(0x14, ni + (si << 6))
        self._wr_mask(0x12, 0x08 if sdm == 0 else 0x00, 0x18)
        self._wr(0x16, sdm >> 8)
        self._wr(0x15, sdm & 0xFF)

        time.sleep(0.002)
        locked = False
        for _ in range(2):
            if self._rd(0x00, 3)[2] & 0x40:
                locked = True; break
            self._wr_mask(0x12, 0x60, 0xE0)           # bump VCO current to max, retry
        self._wr_mask(0x1A, 0x08, 0x08)               # autotune 8 kHz
        print(f"  LO={lo/1e6:.4f} MHz ref={pll_ref/1e6:.3f} mix_div={mix_div} "
              f"nint={nint} sdm={sdm} lock={'YES' if locked else 'NO'}")
        return locked

    # --- R828D standby (port of r82xx_standby) ---
    def r828d_standby(self):
        for reg, val in ((0x06,0xB1),(0x05,0xA0),(0x07,0x3A),(0x08,0x40),(0x09,0xC0),
                         (0x0A,0x36),(0x0C,0x35),(0x0F,0x68),(0x11,0x03),(0x17,0xF4),
                         (0x19,0x0C)):
            self._wr(reg, val)

    # --- path + lifecycle ---
    def enter_vhf(self, base, bias_tee):
        word = (base | VHF_EN) & ~BIAS_HF
        if bias_tee: word |= BIAS_VHF
        self.gpio(word)
        return word

    def standby(self, base):
        self.r828d_standby()
        self.clkb_off()
        self.gpio((base | BIAS_HF) & ~VHF_EN & ~BIAS_VHF)


def firmware_load(img, fx3_cmd="fx3_cmd", timeout=15):
    """Prerequisite: load firmware if the device is in bootloader (04B4:00F3).
    Delegates to the proven `fx3_cmd load` rather than reimplementing the FX3
    RAM-download protocol, then waits for re-enumeration to 04B4:00F1."""
    if usb.core.find(idVendor=RX888_VID, idProduct=RX888_PID) is not None:
        print("load: already in app mode (00F1) — skipping"); return
    if usb.core.find(idVendor=RX888_VID, idProduct=RX888_PID_BOOT) is None:
        sys.exit("load: device not found in bootloader (00F3) or app mode (00F1)")
    print(f"load: {fx3_cmd} load {img}")
    r = subprocess.run([fx3_cmd, "load", img])
    if r.returncode != 0:
        sys.exit(f"load: '{fx3_cmd} load' failed (rc={r.returncode})")
    deadline = time.time() + timeout         # wait for re-enumeration to 00F1
    while time.time() < deadline:
        if usb.core.find(idVendor=RX888_VID, idProduct=RX888_PID) is not None:
            print("load: re-enumerated as 00F1"); return
        time.sleep(0.3)
    sys.exit("load: device did not re-enumerate to app mode (00F1)")


def main():
    ap = argparse.ArgumentParser(description="RX888 mk2 VHF tune (host-side, EP0)")
    ap.add_argument("rf_hz", type=float, help="VHF RF frequency to tune, Hz")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=None,
                    help="HF GPIO control word (hex) to build on; default: read "
                         "the live word from GETSTATS so the running app's "
                         "ADC/GPIO settings are preserved")
    ap.add_argument("--ref", type=float, default=R828D_REF_HZ,
                    help=f"R828D reference via Si5351 CLKB, Hz (default "
                         f"{R828D_REF_HZ}); the PLL math uses this value too")
    ap.add_argument("--bias-tee", action="store_true", help="VHF-port DC bias")
    ap.add_argument("--persist", action="store_true",
                    help="leave the tune active and exit (no standby on exit)")
    ap.add_argument("--standby", action="store_true",
                    help="just standby (R828D off + CLKB off + HF) and exit")
    ap.add_argument("--force", action="store_true",
                    help="proceed despite an R828D ID mismatch or PLL lock "
                         "failure (debug only — may hold a bad tune)")
    # Prerequisites the script can run for you:
    ap.add_argument("--load", metavar="IMG",
                    help="load firmware if in bootloader (via `fx3_cmd load IMG`)")
    ap.add_argument("--fx3-cmd", default="fx3_cmd",
                    help="path to the fx3_cmd binary used by --load")
    ap.add_argument("--adc", metavar="RATE_HZ", type=float,
                    help="run STARTADC at RATE_HZ first (sets the ADC sample "
                         "clock; required before STARTFX3 streaming)")
    args = ap.parse_args()

    if usb is None:
        sys.exit("pyusb is required: pip install pyusb")

    if args.load:                                 # prerequisite 1: firmware
        firmware_load(args.load, args.fx3_cmd)

    rx = RX888()
    rx.check_alive()                              # firmware responds? (beyond lsusb)

    # Preserve the running app's GPIO (ADC dither/rand, PGA, ...) unless told
    # otherwise — flip only VHF_EN/bias on top.
    if args.base is not None:
        base = args.base
        print(f"base GPIO 0x{base:05X} (given)")
    else:
        base = rx.read_gpio_state()
        if base is None:
            sys.exit(
                "this firmware's GETSTATS does not expose GPIO state (release "
                "v0.1.0 / fw 2.3 and earlier), so the script can't read the "
                "running app's GPIO to preserve it. Pass --base <your HF GPIO "
                "control word in hex> — without it a whole-word write would "
                "zero the app's ADC/GPIO settings (DITH/RANDO/PGA/...).")
        print(f"base GPIO 0x{base:05X} (read live from GETSTATS)")

    if args.standby:
        rx.standby(base); print("standby: R828D off, CLKB off, HF"); return

    if args.adc:                                  # prerequisite 2: ADC sample clock
        rx.start_adc(int(args.adc)); print(f"STARTADC = {args.adc/1e6:.3f} MHz")

    # --- bring-up lifecycle: any failure/exception standbys unless we persist ---
    failed = None
    leave_active = False
    try:
        print(f"enter VHF: GPIO 0x{rx.enter_vhf(base, args.bias_tee):05X}")
        rx.clkb_on(int(args.ref)); print(f"CLKB = {args.ref/1e6:.3f} MHz")
        rx.clkb_verify()                          # read back: CLKB actually on?

        idv = rx.r828d_probe()                    # tuner reachable / right chip?
        if idv is None:
            raise TuneError("R828D not reachable over I2C")
        if idv != 0x69 and not args.force:
            raise TuneError(f"R828D ID 0x{idv:02X} != 0x69 (override with --force)")

        rx.r828d_init(); print("R828D init + 8 MHz filter (IF 4.57 MHz)")

        if not rx.r828d_set_freq(int(args.rf_hz)) and not args.force:
            raise TuneError("R828D PLL did not lock / out of range "
                            "(override with --force)")
        print(f"tuned {args.rf_hz/1e6:.4f} MHz -> IF ~4.57 MHz; stream the IF")

        if args.persist:
            print("persist: leaving tune active")
            leave_active = True
        else:
            print("holding tune — Ctrl-C to standby and exit")
            signal.signal(signal.SIGINT, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
            while True:
                time.sleep(1)
    except KeyboardInterrupt:
        pass                                      # normal hold exit -> standby below
    except (TuneError, usb.core.USBError) as e:
        failed = str(e)
    finally:
        if not leave_active:
            try:
                rx.standby(base)
                print("standby: R828D off, CLKB off, HF")
            except Exception as e2:               # best-effort cleanup
                print(f"warning: standby cleanup failed: {e2}")

    if failed:
        sys.exit(f"FAIL: {failed}")


if __name__ == "__main__":
    main()
