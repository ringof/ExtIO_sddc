#!/usr/bin/env python3
"""
vhf_tune.py — RX888 mk2 VHF front-end controller, over EP0 vendor commands.

Enter VHF, tune the R828D to a frequency, hold, standby on exit. Runs alongside
a streamer (EP0 control vs EP1 bulk; the two_actor_open pattern, firmware #143).
Failures are fatal by default (override with --force); any error during bring-up
standbys.

This doubles as a worked reference for a C host driver: the RX888 class is the
portable core (EP0 transport + Si5351 CLKB + R828D); main() is host-tool CLI.
Register sequences are ported from the firmware (si5351aSetFrequencyB,
r82xx_init/set_bandwidth/set_mux/set_pll/standby @ 0ffa512) and RX888R2Radio.cpp
(REF=16 MHz, IF=4.57 MHz). Partial port of set_freq64: set_mux + set_pll + input
switch, no harmonic retry (only needed >~1.7 GHz). Bench-validated to lock at
144 MHz; confirm the math outside that band.

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

import argparse, struct, sys, time, signal, subprocess
try:
    import usb.core
except ImportError:
    usb = None

# Device identity
RX888_VID      = 0x04B4
RX888_PID      = 0x00F1          # application firmware
RX888_PID_BOOT = 0x00F3          # Cypress bootloader

# EP0 vendor request codes (protocol.h)
TESTFX3  = 0xAC                  # IN  -> [hwconfig, fw_hi, fw_lo, vendor_rqt_count]
GPIOFX3  = 0xAD                  # OUT <- 4-byte LE GPIO control word
I2CWFX3  = 0xAE                  # OUT <- I2C write: wValue=dev, wIndex=reg, data
I2CRFX3  = 0xAF                  # IN  -> I2C read:  wValue=dev, wIndex=reg, wLength=n
STARTADC = 0xB2                  # OUT <- 4-byte LE ADC sample rate
GETSTATS = 0xB3                  # IN  -> diagnostics; gpio_state at [26:30] (payload >= 30B)
BM_OUT   = 0x40                  # bmRequestType: host->device | vendor | device
BM_IN    = 0xC0                  # bmRequestType: device->host | vendor | device

# GPIO control-word bits (protocol.h enum GPIOPin)
BIAS_HF  = 1 << 8
BIAS_VHF = 1 << 9
VHF_EN   = 1 << 15               # HF/VHF antenna switch (set = VHF)

# I2C device addresses (8-bit, as the firmware's I2cTransfer uses)
R828D_ADDR  = 0x74
SI5351_ADDR = 0xC0

# Board constants (RX888R2Radio.cpp / Si5351.c)
R828D_REF_HZ = 16_000_000        # R828D reference fed via Si5351 CLKB
IF_CARRIER   = 4_570_000         # IF center with the 8 MHz channel filter
SI5351_XTAL  = 27_000_000        # Si5351 crystal

# Si5351 register addresses
SI_PLL_B     = 34
SI_MS2       = 58                # multisynth for CLK2 (= CLKB)
SI_PLL_RESET = 177
SI_CLK2      = 18                # CLK2 control (bit7 = power-down)

# R828D init register block, regs 0x05..0x1f (r82xx_init_array; IF_VGA=11, VER=49)
R828D_INIT_BASE = 0x05
R828D_INIT = [
    0x80,0x13,0x70,0xC0,0x40,0xDB,0x6B,0xEB,0x53,0x75,0x68,0x6C,0xBB,
    0x80,0x31,0x0F,0x00,0xC0,0x30,0x48,0xEC,0x60,0x00,0x24,0xDD,0x0E,0x40,
]

# R828D tracking-filter bands: (LO_start_MHz, open_d, rf_mux_ploy, tf_c)
FREQ_RANGES = [
    (  0,0x08,0x02,0xDF),( 50,0x08,0x02,0xBE),( 55,0x08,0x02,0x8B),( 60,0x08,0x02,0x7B),
    ( 65,0x08,0x02,0x69),( 70,0x08,0x02,0x58),( 75,0x00,0x02,0x44),( 80,0x00,0x02,0x44),
    ( 90,0x00,0x02,0x34),(100,0x00,0x02,0x34),(110,0x00,0x02,0x24),(120,0x00,0x02,0x24),
    (140,0x00,0x02,0x14),(180,0x00,0x02,0x13),(220,0x00,0x02,0x13),(250,0x00,0x02,0x11),
    (280,0x00,0x02,0x00),(310,0x00,0x41,0x00),(450,0x00,0x41,0x00),(588,0x00,0x40,0x00),
    (650,0x00,0x40,0x00),
]


class TuneError(Exception):
    """Bring-up failure (unreachable tuner, ID mismatch, no PLL lock, ...)."""


def _bitrev8(b):
    """Reverse the 8 bits of a byte. The R828D streams reads LSB-first, so a
    standard MSB-first I2C master (the FX3) receives each read byte reversed
    from the datasheet's logical bit order; this undoes that. Writes are NOT
    reversed (the chip takes writes MSB-first). Mirrors librtlsdr r82xx_read."""
    b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4)
    b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2)
    b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1)
    return b


# ════════════════════════════════════════════════════════════════════════
#  Portable driver core — this is what a C host driver re-implements.
# ════════════════════════════════════════════════════════════════════════
class RX888:
    def __init__(self):
        self.dev = usb.core.find(idVendor=RX888_VID, idProduct=RX888_PID)
        if self.dev is None:
            sys.exit("RX888 mk2 not found (04B4:00F1).")
        self.regs = {}            # R828D register shadow (for masked writes)
        self.ref_hz = R828D_REF_HZ   # active R828D reference (CLKB); set by clkb_on

    # ── EP0 transport (libusb control transfers) ──────────────────────────
    def _out_u32(self, cmd, value):              # GPIOFX3 / STARTADC payload
        self.dev.ctrl_transfer(BM_OUT, cmd, 0, 0, struct.pack("<I", value))

    def gpio(self, word):     self._out_u32(GPIOFX3, word)
    def start_adc(self, hz):  self._out_u32(STARTADC, hz)

    def i2c_w(self, addr, reg, data):
        self.dev.ctrl_transfer(BM_OUT, I2CWFX3, addr, reg, bytes(data))

    def i2c_r(self, addr, reg, n):
        return bytes(self.dev.ctrl_transfer(BM_IN, I2CRFX3, addr, reg, n))

    # ── diagnostics / verification ────────────────────────────────────────
    def check_alive(self):
        """Confirm the firmware answers vendor commands (beyond lsusb)."""
        try:
            hw, fwhi, fwlo, rqt = bytes(self.dev.ctrl_transfer(BM_IN, TESTFX3, 0, 0, 4))
        except usb.core.USBError as e:
            sys.exit(f"RX888 on USB but firmware not answering TESTFX3 ({e}). "
                     f"Powered up? claimed elsewhere?")
        print(f"alive: hwconfig=0x{hw:02X} fw={fwhi}.{fwlo} vendor_rqts={rqt}")
        if hw != 0x04:
            print(f"  WARNING: hwconfig 0x{hw:02X} is not RX888r2 (0x04)")
        return hw

    def read_gpio_state(self):
        """Live steady-state GPIO word from GETSTATS bytes [26:30] (packed with
        the GPIOFX3 bit positions). Returns None if the payload predates that
        field (< 30 bytes, e.g. release v0.1.0) so the caller requires --base."""
        buf = bytes(self.dev.ctrl_transfer(BM_IN, GETSTATS, 0, 0, 64))
        return int.from_bytes(buf[26:30], "little") if len(buf) >= 30 else None

    def clkb_verify(self):
        """Read back Si5351 CLK2_CONTROL (bit7 = power-down) — True iff enabled."""
        v = self.i2c_r(SI5351_ADDR, SI_CLK2, 1)[0]
        on = not (v & 0x80)
        print(f"  CLKB verify: CLK2_CTRL=0x{v:02X} -> {'enabled' if on else 'OFF'}")
        return on

    def r828d_probe(self):
        """Read the R828D ID (reg 0x00) in logical order. Returns the byte
        (0x96 expected — the datasheet's logical chip-id), or None if it doesn't
        ACK. _rd bit-reverses the wire 0x69 back to logical 0x96."""
        try:
            v = self._rd(0x00, 1)[0]
        except usb.core.USBError as e:
            print(f"  R828D probe: no I2C response ({e}) — tuner unreachable")
            return None
        print(f"  R828D probe: reg0=0x{v:02X} (want 0x96) -> "
              f"{'OK' if v == 0x96 else 'ID MISMATCH'}")
        return v

    # ── R828D register access (shadowed, like the firmware's priv->regs) ──
    def _wr(self, reg, val):
        self.regs[reg] = val & 0xFF
        self.i2c_w(R828D_ADDR, reg, [val & 0xFF])

    def _wr_mask(self, reg, val, mask):          # read-modify-write vs the shadow
        self._wr(reg, (self.regs.get(reg, 0) & ~mask) | (val & mask))

    def _rd(self, reg, n):
        """Read n R828D registers from reg, in datasheet/librtlsdr LOGICAL bit
        order. The chip streams reads LSB-first so each wire byte is reversed;
        _bitrev8 undoes it. i2c_r stays raw (the Si5351 reads normally), so the
        reverse lives only on this R828D read path."""
        return [_bitrev8(v) for v in self.i2c_r(R828D_ADDR, reg, n)]

    # ── Si5351 CLKB = ref_hz on CLK2/PLL-B (port of si5351aSetFrequencyB) ──
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
        # PLL: a=mult. MS: a=divider, rdiv given. P1/P2/P3 (SetupPLL/SetupMultisynth).
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

    # ── R828D init (port of r82xx_init + set_bandwidth(8 MHz)) ─────────────
    def r828d_init(self):
        for i, v in enumerate(R828D_INIT):           # seed shadow + write 0x05..0x1f
            self.regs[R828D_INIT_BASE + i] = v
            self.i2c_w(R828D_ADDR, R828D_INIT_BASE + i, [v])
        # set_bandwidth(8 MHz): IF channel filter -> IF center 4.57 MHz
        self._wr_mask(0x0A, 0x10, 0x0F)
        self._wr_mask(0x0B, 0x0B, 0xEF)
        self._wr_mask(0x1E, 0x60, 0x40)

    # ── R828D tune (partial port of set_freq64: set_mux + set_pll + input sw) ─
    def r828d_set_freq(self, rf_hz):
        """LO = RF + IF (low-side). set_mux + set_pll, then the Air-In/Cable1
        input switch. No harmonic retry (out of VHF scope). True iff PLL locked."""
        lo = rf_hz + IF_CARRIER
        self._set_mux(lo)
        ok = self._set_pll(lo)
        self._wr_mask(0x05, 0x00 if rf_hz > 345_000_000 else 0x60, 0x60)  # Air/Cable
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
        self._wr_mask(0x10, 0x08, 0x0B)               # default xtal cap (0pF | drive)

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
        vco_fine = (data[4] & 0x30) >> 4              # reg 0x04 logical b5:4
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
        self._wr(0x14, ni + (si << 6))                # nint
        self._wr_mask(0x12, 0x08 if sdm == 0 else 0x00, 0x18)
        self._wr(0x16, sdm >> 8)                       # sdm hi
        self._wr(0x15, sdm & 0xFF)                     # sdm lo

        time.sleep(0.002)
        locked = False
        for _ in range(2):
            if self._rd(0x00, 3)[2] & 0x40:           # lock = reg 0x02 logical b6 (reads are logical-order)
                locked = True; break
            self._wr_mask(0x12, 0x60, 0xE0)           # bump VCO current to max, retry
        self._wr_mask(0x1A, 0x08, 0x08)               # autotune 8 kHz
        print(f"  LO={lo/1e6:.4f} MHz ref={pll_ref/1e6:.3f} mix_div={mix_div} "
              f"nint={nint} sdm={sdm} lock={'YES' if locked else 'NO'}")
        return locked

    # ── R828D standby (port of r82xx_standby) ─────────────────────────────
    def r828d_standby(self):
        for reg, val in ((0x06,0xB1),(0x05,0xA0),(0x07,0x3A),(0x08,0x40),(0x09,0xC0),
                         (0x0A,0x36),(0x0C,0x35),(0x0F,0x68),(0x11,0x03),(0x17,0xF4),
                         (0x19,0x0C)):
            self._wr(reg, val)

    # ── antenna path + lifecycle ──────────────────────────────────────────
    def enter_vhf(self, base, bias_tee):
        word = (base | VHF_EN) & ~BIAS_HF
        if bias_tee: word |= BIAS_VHF
        self.gpio(word)
        return word

    def standby(self, base):
        """Best-effort teardown: attempt every step even if an earlier one
        raises, so a failed R828D standby still turns CLKB off and restores the
        HF GPIO. Prints its own status; returns True iff every step succeeded."""
        errs = []
        for name, step in (
                ("R828D standby", self.r828d_standby),
                ("CLKB off",      self.clkb_off),
                ("HF GPIO",       lambda: self.gpio((base | BIAS_HF) & ~VHF_EN & ~BIAS_VHF))):
            try:
                step()
            except Exception as e:
                errs.append(f"{name}: {e!r}")
        if errs:
            print("  standby PARTIAL — " + "; ".join(errs))
            return False
        print("standby: R828D off, CLKB off, HF")
        return True


# ════════════════════════════════════════════════════════════════════════
#  Host-tool CLI — not part of the portable driver.
# ════════════════════════════════════════════════════════════════════════
def firmware_load(img, fx3_cmd="fx3_cmd", timeout=15):
    """Load firmware if the device is in bootloader (04B4:00F3) by delegating to
    the proven `fx3_cmd load`, then wait for re-enumeration to 04B4:00F1."""
    if usb.core.find(idVendor=RX888_VID, idProduct=RX888_PID) is not None:
        print("load: already in app mode (00F1) — skipping"); return
    if usb.core.find(idVendor=RX888_VID, idProduct=RX888_PID_BOOT) is None:
        sys.exit("load: device not found in bootloader (00F3) or app mode (00F1)")
    print(f"load: {fx3_cmd} load {img}")
    if subprocess.run([fx3_cmd, "load", img]).returncode != 0:
        sys.exit(f"load: '{fx3_cmd} load' failed")
    deadline = time.time() + timeout
    while time.time() < deadline:
        if usb.core.find(idVendor=RX888_VID, idProduct=RX888_PID) is not None:
            print("load: re-enumerated as 00F1"); return
        time.sleep(0.3)
    sys.exit("load: device did not re-enumerate to app mode (00F1)")


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
