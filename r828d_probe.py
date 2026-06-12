#!/usr/bin/env python3
"""
r828d_probe.py — investigate the R828D tuner in an RX888 mk2 over EP0 I2C
passthrough. Characterizes the read interface, dumps the register space, maps
which register bits are writable vs read-only/forced, and scans for registers
beyond the documented 0x05..0x1f.

This generates first-party facts about *your* specific tuner — the cleanest
possible input for an independent driver: you're measuring the chip, not reading
anyone's code.

SAFETY: touches only the R828D (I2C 0x74); never the Si5351 / ADC clock. The
settability probe writes test patterns to tuner registers and restores each one
immediately, re-checking the chip ID (0x69) after every register so a bad write
is caught. It scrambles any active VHF tune (re-tune afterward) but does NOT
affect HF direct-sampling streaming (the tuner is off-path there). Best run when
not VHF-tuned. Aborts unless reg 0x00 reads 0x69 first.

Requires: pyusb  (pip install pyusb)
"""

import argparse, sys
try:
    import usb.core
except ImportError:
    usb = None

RX888_VID, RX888_PID = 0x04B4, 0x00F1
TESTFX3, I2CWFX3, I2CRFX3 = 0xAC, 0xAE, 0xAF
BM_OUT, BM_IN = 0x40, 0xC0
R828D_ADDR = 0x74

# R828D init defaults (regs 0x05..0x1f) — used to restore a sane state at the end.
R828D_INIT_BASE = 0x05
R828D_INIT = [0x80,0x13,0x70,0xC0,0x40,0xDB,0x6B,0xEB,0x53,0x75,0x68,0x6C,0xBB,
              0x80,0x31,0x0F,0x00,0xC0,0x30,0x48,0xEC,0x60,0x00,0x24,0xDD,0x0E,0x40]


class Tuner:
    def __init__(self):
        self.dev = usb.core.find(idVendor=RX888_VID, idProduct=RX888_PID)
        if self.dev is None:
            sys.exit("RX888 mk2 not found (04B4:00F1).")
        self.read_mode = None         # 'random' | 'from0' — set by characterize_reads()

    # ── EP0 / I2C transport ───────────────────────────────────────────────
    def alive(self):
        try:
            info = bytes(self.dev.ctrl_transfer(BM_IN, TESTFX3, 0, 0, 4))
        except usb.core.USBError as e:
            sys.exit(f"firmware not answering TESTFX3 ({e})")
        print(f"alive: hwconfig=0x{info[0]:02X} fw={info[1]}.{info[2]}")

    def i2c_w(self, reg, data):
        self.dev.ctrl_transfer(BM_OUT, I2CWFX3, R828D_ADDR, reg, bytes(data))

    def i2c_r(self, reg, n):
        try:
            return list(self.dev.ctrl_transfer(BM_IN, I2CRFX3, R828D_ADDR, reg, n))
        except usb.core.USBError:
            return None               # NAK / no response

    # ── read-interface characterization ───────────────────────────────────
    def characterize_reads(self):
        """The R82xx classically ignores the read register-pointer and always
        streams from reg 0. The FX3 I2CRFX3 path may differ — find out.

        A = read(reg=0, n=1); B = read(reg=5, n=1); C = read(reg=0, n=6).
          B == C[5]            -> random access works (byteAddress honored)
          B == C[0] (== A)     -> pointer ignored, reads always start at 0
        """
        A = self.i2c_r(0x00, 1)
        B = self.i2c_r(0x05, 1)
        C = self.i2c_r(0x00, 6)
        if not (A and B and C and len(C) == 6):
            sys.exit("characterize_reads: I2C reads failed — is the tuner present?")
        if B[0] == C[5] and B[0] != C[0]:
            self.read_mode = "random"
        elif B[0] == C[0]:
            self.read_mode = "from0"
        else:
            self.read_mode = "from0"  # ambiguous; the safe assumption
        print(f"read interface: reg0=0x{A[0]:02X}  read(5,1)=0x{B[0]:02X}  "
              f"read(0,6)[5]=0x{C[5]:02X}  ->  mode = {self.read_mode}"
              + ("  (byteAddress honored)" if self.read_mode == "random"
                 else "  (reads start at reg 0; index into a block)"))

    def rd(self, reg):
        """Read one register, honoring the characterized read mode."""
        if self.read_mode == "random":
            v = self.i2c_r(reg, 1)
            return v[0] if v else None
        blk = self.i2c_r(0x00, reg + 1)            # from0: read 0..reg, take last
        return blk[reg] if blk and len(blk) > reg else None

    def rd_block(self, lo, hi):
        if self.read_mode == "random":
            return self.i2c_r(lo, hi - lo + 1) or []
        blk = self.i2c_r(0x00, hi + 1) or []
        return blk[lo:hi + 1]

    # ── register-space dump ───────────────────────────────────────────────
    def dump(self, lo=0x00, hi=0x1F):
        print(f"\n=== register dump 0x{lo:02X}..0x{hi:02X} ===")
        vals = self.rd_block(lo, hi)
        for i, v in enumerate(vals):
            reg = lo + i
            print(f"  reg 0x{reg:02X} = 0x{v:02X}  {v:08b}")
        return vals

    # ── per-bit settability map ───────────────────────────────────────────
    def settability(self, lo=0x00, hi=0x1F):
        """Write-probe EVERY register 0x00..0x1f: write 0x00 then 0xFF, read
        back, classify each bit, restore the original. The read-only status
        registers 0x00..0x04 should come back F1/F0 (fixed bits) — which also
        validates that the classifier can actually tell read-only from writable.
        Re-checks the chip ID after each register and stops if it changes."""
        print(f"\n=== settability 0x{lo:02X}..0x{hi:02X} "
              f"(RW=writable, F1=forced-1, F0=forced-0, ~=inverted/status) ===")
        for reg in range(lo, hi + 1):
            orig = self.rd(reg)
            if orig is None:
                print(f"  reg 0x{reg:02X}: not readable, skipped"); continue
            self.i2c_w(reg, [0x00]); z = self.rd(reg)
            self.i2c_w(reg, [0xFF]); o = self.rd(reg)
            self.i2c_w(reg, [orig])                     # restore
            if z is None or o is None:
                print(f"  reg 0x{reg:02X}: readback failed during probe"); continue
            rw  = [i for i in range(8) if not (z >> i) & 1 and (o >> i) & 1]
            f1  = [i for i in range(8) if (z >> i) & 1 and (o >> i) & 1]
            f0  = [i for i in range(8) if not (z >> i) & 1 and not (o >> i) & 1]
            inv = [i for i in range(8) if (z >> i) & 1 and not (o >> i) & 1]
            print(f"  reg 0x{reg:02X}: orig=0x{orig:02X} z=0x{z:02X} o=0x{o:02X} | "
                  f"RW={_bits(rw)} F1={_bits(f1)} F0={_bits(f0)} ~={_bits(inv)}")
            if self.rd(0x00) != 0x69:                   # sanity after each register
                print("  !! chip ID no longer 0x69 — stopping; re-init recommended")
                return

    # ── scan for registers beyond the documented map ──────────────────────
    def scan(self, start=0x20, end=0x3F):
        """Probe registers past 0x1f. Only meaningful in 'random' read mode — in
        'from0' the address space wraps, so reads alias the low map and writes to
        high addresses would clobber the low registers."""
        if self.read_mode == "from0":
            blk = self.i2c_r(0x00, 64) or []
            win = next((p for p in range(8, len(blk))
                        if blk[p] == blk[0] and blk[p:p + 8] == blk[0:8]), None)
            print("\n=== scan (from0 read mode) ===")
            if win:
                print(f"  read window = {win} registers (0x00..0x{win - 1:02X}); "
                      f"reads beyond it wrap back to reg 0 — no extended registers "
                      f"are reachable by sequential read on this interface.")
            else:
                print("  could not determine the wrap window from a 64-byte read.")
            print("  skipping write-probe: in from0 mode a write to 0x20+ wraps "
                  "onto the low registers (clobber risk).")
            return

        print(f"\n=== scan 0x{start:02X}..0x{end:02X} for undocumented registers ===")
        base = self.rd_block(0x00, 0x1F)               # for wrap detection
        for reg in range(start, end + 1):
            v = self.rd(reg)
            if v is None:
                print(f"  reg 0x{reg:02X}: no read"); continue
            wraps = base and (reg & 0x1F) < len(base) and v == base[reg & 0x1F]
            if wraps:
                print(f"  reg 0x{reg:02X} = 0x{v:02X}  -> wraps to low map"); continue
            orig = v
            self.i2c_w(reg, [orig ^ 0xFF]); rb = self.rd(reg)
            self.i2c_w(reg, [orig])
            holds = rb is not None and rb != orig
            print(f"  reg 0x{reg:02X} = 0x{v:02X}  -> "
                  f"{'HOLDS a write!' if holds else 'static'}")

    def reinit(self):
        """Restore the documented init defaults (sane post-probe state)."""
        for i, val in enumerate(R828D_INIT):
            self.i2c_w(R828D_INIT_BASE + i, [val])
        print("re-init: wrote documented defaults to 0x05..0x1f")


def _bits(lst):
    return "{" + ",".join(str(b) for b in lst) + "}" if lst else "{}"


def main():
    ap = argparse.ArgumentParser(description="Investigate the RX888 mk2 R828D tuner")
    ap.add_argument("--dump",  action="store_true", help="read+print regs 0x00..0x1f")
    ap.add_argument("--settability", action="store_true",
                    help="map per-bit writable vs read-only (writes+restores)")
    ap.add_argument("--scan",  action="store_true",
                    help="probe regs 0x20..0x3f for undocumented registers")
    ap.add_argument("--all", action="store_true", help="dump + settability + scan")
    ap.add_argument("--no-reinit", action="store_true",
                    help="do not restore init defaults at the end")
    args = ap.parse_args()
    if usb is None:
        sys.exit("pyusb is required: pip install pyusb")
    if not (args.dump or args.settability or args.scan or args.all):
        ap.error("pick at least one of --dump / --settability / --scan / --all")

    t = Tuner()
    t.alive()
    t.characterize_reads()
    if t.rd(0x00) != 0x69:
        sys.exit(f"reg 0x00 != 0x69 (got 0x{t.rd(0x00):02X}) — not an R828D / not reachable; "
                 "refusing to write")

    wrote = False
    if args.dump or args.all:
        t.dump(0x00, 0x1F)
    if args.settability or args.all:
        t.settability(0x00, 0x1F); wrote = True
    if args.scan or args.all:
        t.scan(); wrote = True

    if wrote and not args.no_reinit:
        t.reinit()


if __name__ == "__main__":
    main()
