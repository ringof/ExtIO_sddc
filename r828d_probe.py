#!/usr/bin/env python3
"""
r828d_probe.py — investigate the R828D tuner in an RX888 mk2 over EP0 I2C
passthrough. Characterizes the read interface, dumps the register space, maps
which register bits are writable vs read-only/forced, scans for registers
beyond the documented 0x05..0x1f, and snapshots/diffs the register space to see
which bits move across an external operation (e.g. a tune).

This generates first-party facts about *your* specific tuner — the cleanest
possible input for an independent driver: you're measuring the chip, not reading
anyone's code.

BIT ORDER: the R820T2/R828D streams reads LSB-first but takes writes MSB-first
(Rafael "R820T2 Register Description", read-mode vs write-mode sections). A
standard MSB-first I2C master (the FX3) therefore receives every read byte
*bit-reversed* from the datasheet's logical numbering — which is why reg 0x00
reads 0x69 here while the datasheet matrix lists it as 0x96. So in this tool's
output a read "wire bit w" equals datasheet "logical bit 7-w". Writes are NOT
reversed (we send librtlsdr's logical init values verbatim and the chip stores
them as-is). The --diff lock analysis spells this out for reg 0x02.

SAFETY: touches only the R828D (I2C 0x74); never the Si5351 / ADC clock. The
settability probe writes test patterns to tuner registers and restores each one
immediately, re-checking the chip ID (0x69) after every register so a bad write
is caught. It scrambles any active VHF tune (re-tune afterward) but does NOT
affect HF direct-sampling streaming (the tuner is off-path there). Best run when
not VHF-tuned. Aborts unless reg 0x00 reads 0x69 first. --snapshot and --diff
are READ-ONLY (safe to run while streaming, and against a live VHF tune).

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

# Datasheet names for the read/status registers (Rafael R820T2 Register Matrix,
# table 1-2). Annotations in --diff so a changed bit can be read against the doc.
DOC_READ_FIELDS = {
    0x00: "R0  chip-id (logical 0x96 / wire 0x69)",
    0x01: "R1  reserved (undocumented in datasheet)",
    0x02: "R2  VCO_INDICATOR[6:0] (logical b6:0; logical b7=0)",
    0x03: "R3  RF_INDICATOR[7:0]",
    0x04: "R4  reserved (librtlsdr reads logical b5:4 as vco_fine)",
}


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

    # ── snapshot (read-only) ──────────────────────────────────────────────
    def read_all(self):
        """Read 0x00..0x1f as a {reg: value} dict (read-only, wire order)."""
        vals = self.rd_block(0x00, 0x1F)
        return {0x00 + i: v for i, v in enumerate(vals)}

    def save_snapshot(self, path, label=""):
        regs = self.read_all()
        with open(path, "w") as f:
            f.write("# r828d snapshot — WIRE order "
                    "(bit-reversed from Rafael logical numbering; wire b_w = logical b_7-w)\n")
            if label:
                f.write(f"# label: {label}\n")
            for reg in sorted(regs):
                f.write(f"0x{reg:02X} 0x{regs[reg]:02X}\n")
        print(f"snapshot: wrote 0x00..0x1F to {path}" + (f"  [{label}]" if label else ""))
        for reg in sorted(regs):
            print(f"  reg 0x{reg:02X} = 0x{regs[reg]:02X}  {regs[reg]:08b}")

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


def load_snapshot(path):
    """Parse a snapshot file written by save_snapshot(). No device needed."""
    regs, label = {}, ""
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line.startswith("# label:"):
                    label = line.split(":", 1)[1].strip(); continue
                if not line or line.startswith("#"):
                    continue
                tok = line.split()
                regs[int(tok[0], 0)] = int(tok[1], 0)
    except OSError as e:
        sys.exit(f"cannot read snapshot {path}: {e}")
    if not regs:
        sys.exit(f"snapshot {path} had no register lines")
    return regs, label


def diff_snapshots(a, b, name_a, name_b):
    """Show which bits moved between two snapshots (both in wire order)."""
    print(f"\n=== diff  {name_a}  ->  {name_b} ===")
    print("  reads are WIRE order: wire bit w = datasheet logical bit 7-w")
    changed = False
    for reg in range(0x00, 0x20):
        va, vb = a.get(reg), b.get(reg)
        if va is None or vb is None:
            continue
        x = va ^ vb
        if not x:
            continue
        changed = True
        wbits = sorted(i for i in range(8) if (x >> i) & 1)
        lbits = sorted(7 - i for i in wbits)
        note = DOC_READ_FIELDS.get(reg, "")
        print(f"  reg 0x{reg:02X}: 0x{va:02X} -> 0x{vb:02X}  "
              f"wire {_bits(wbits)} = logical {_bits(lbits)}"
              + (f"  | {note}" if note else ""))
    if not changed:
        print("  (no bits changed)")
    _lock_analysis(a, b)


def _lock_analysis(a, b):
    """Resolve the reg-0x02 PLL-lock-bit question: which bit actually transitions.

    vhf_tune.py checks `data[2] & 0x40`. In wire order that mask is wire bit 6 =
    logical VCO_INDICATOR[1]. The datasheet's lock-ish MSB is VCO_INDICATOR[6] =
    logical bit 6 = wire bit 1 = mask 0x02. Whichever goes 0->1 on a known-good
    tune is the real lock indicator."""
    va, vb = a.get(0x02), b.get(0x02)
    if va is None or vb is None:
        return
    bit = lambda v, m: 1 if v & m else 0
    print("\n  PLL-lock bit hypothesis (reg 0x02):")
    print(f"    script's current check  mask 0x40 (wire b6 = logical VCO_INDICATOR[1]): "
          f"{bit(va, 0x40)} -> {bit(vb, 0x40)}")
    print(f"    datasheet lock candidate mask 0x02 (wire b1 = logical VCO_INDICATOR[6]): "
          f"{bit(va, 0x02)} -> {bit(vb, 0x02)}")
    print("    -> the bit that transitions 0->1 on a known-good tune is the real lock bit.")


def main():
    ap = argparse.ArgumentParser(description="Investigate the RX888 mk2 R828D tuner")
    ap.add_argument("--dump",  action="store_true", help="read+print regs 0x00..0x1f")
    ap.add_argument("--settability", action="store_true",
                    help="map per-bit writable vs read-only (writes+restores)")
    ap.add_argument("--scan",  action="store_true",
                    help="probe regs 0x20..0x3f for undocumented registers")
    ap.add_argument("--all", action="store_true", help="dump + settability + scan")
    ap.add_argument("--snapshot", metavar="PATH",
                    help="read 0x00..0x1f and save to PATH (read-only; safe while streaming)")
    ap.add_argument("--diff", nargs=2, metavar=("BEFORE", "AFTER"),
                    help="diff two snapshot files; shows moved bits + PLL-lock analysis "
                         "(no device needed)")
    ap.add_argument("--label", default="",
                    help="optional label stored in the --snapshot file")
    ap.add_argument("--no-reinit", action="store_true",
                    help="do not restore init defaults at the end")
    args = ap.parse_args()

    # --diff is a pure file operation: no device, no pyusb.
    if args.diff:
        a, la = load_snapshot(args.diff[0])
        b, lb = load_snapshot(args.diff[1])
        diff_snapshots(a, b,
                       args.diff[0] + (f" [{la}]" if la else ""),
                       args.diff[1] + (f" [{lb}]" if lb else ""))
        return

    if usb is None:
        sys.exit("pyusb is required: pip install pyusb")
    if not (args.dump or args.settability or args.scan or args.all or args.snapshot):
        ap.error("pick at least one of --dump / --settability / --scan / --all "
                 "/ --snapshot / --diff")

    t = Tuner()
    t.alive()
    t.characterize_reads()
    if t.rd(0x00) != 0x69:
        sys.exit(f"reg 0x00 != 0x69 (got 0x{t.rd(0x00):02X}) — not an R828D / not reachable; "
                 "refusing to write")

    if args.snapshot:
        t.save_snapshot(args.snapshot, args.label)

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
