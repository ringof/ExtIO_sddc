#!/usr/bin/env python3
"""
vhf_tune.py — minimal RX888 mk2 VHF front-end config utility.

Configures the analog front-end for VHF entirely over EP0 vendor control
transfers, then exits. The front-end state persists in hardware (GPIO latch,
Si5351 + R828D registers), so a streamer (rx888_stream / ka9q-radio) reading
EP1 sees the configured IF. Control (EP0) and stream (EP1) are independent and
vendor/device-recipient control needs no interface claim, so this can run
*alongside* a live stream (the two_actor_open pattern, firmware test #143).

What is real here: device open, the EP0 command helpers, the GPIO VHF switch,
and the R828D reachability probe — all grounded in the firmware's actual
command formats.

What is STUBBED (fill from the bench / clean upstream — see vhf_host_bringup.md):
  * Si5351 CLK2/CLKB programming  -> multisynth registers for your reference
  * R828D init + set_freq         -> steve-m/librtlsdr src/tuner_r82xx.c

Requires: pyusb  (pip install pyusb)
"""

import argparse
import struct
import sys
import usb.core
import usb.util

# ---- device ----------------------------------------------------------------
RX888_VID = 0x04B4
RX888_PID = 0x00F1          # application mode (0x00F3 = bootloader)

# ---- EP0 vendor commands (protocol.h) --------------------------------------
GPIOFX3   = 0xAD            # OUT, 4-byte LE control word in data phase
I2CWFX3   = 0xAE            # OUT, wValue=devaddr wIndex=reg, data=payload
I2CRFX3   = 0xAF            # IN,  wValue=devaddr wIndex=reg, wLength=count
STARTADC  = 0xB2
GETSTATS  = 0xB3

BM_OUT = 0x40              # host->device | vendor | device
BM_IN  = 0xC0             # device->host | vendor | device

# ---- GPIO control-word bits (protocol.h enum GPIOPin) ----------------------
BIAS_HF  = 1 << 8         # 0x0100  HF antenna bias-tee
BIAS_VHF = 1 << 9         # 0x0200  VHF antenna bias-tee
VHF_EN   = 1 << 15        # 0x8000  HF/VHF RF switch  (set = VHF)

# ---- I2C addresses ---------------------------------------------------------
R828D_ADDR = 0x74         # 8-bit form, as the firmware uses it
SI5351_ADDR = 0xC0        # Si5351 (8-bit form used by the firmware's I2cTransfer)
R828D_ID_REG = 0x00
R828D_ID_VAL = 0x69       # expected after the R82xx read bit-reversal (raw byte = 0x96)


def _bitrev8(b):
    """R82xx returns register reads bit-reversed within each byte."""
    return int(f"{b:08b}"[::-1], 2)


class RX888:
    def __init__(self):
        self.dev = usb.core.find(idVendor=RX888_VID, idProduct=RX888_PID)
        if self.dev is None:
            sys.exit("RX888 mk2 not found (04B4:00F1). In bootloader (00F3)? "
                     "Upload firmware first.")
        # NOTE: deliberately do NOT claim the interface — vendor/device-recipient
        # EP0 control works without it, so a streamer can hold EP1 concurrently.

    # --- EP0 primitives -----------------------------------------------------
    def gpio(self, word):
        """GPIOFX3: whole 32-bit steady-state GPIO word, LE, in the data phase."""
        self.dev.ctrl_transfer(BM_OUT, GPIOFX3, 0, 0, struct.pack("<I", word))

    def i2c_write(self, dev_addr, reg, data):
        """I2CWFX3: wValue=dev_addr, wIndex=reg, payload=data (bytes)."""
        self.dev.ctrl_transfer(BM_OUT, I2CWFX3, dev_addr, reg, bytes(data))

    def i2c_read(self, dev_addr, reg, count):
        """I2CRFX3: wValue=dev_addr, wIndex=reg, wLength=count -> bytes."""
        return bytes(self.dev.ctrl_transfer(BM_IN, I2CRFX3, dev_addr, reg, count))

    # --- front-end ----------------------------------------------------------
    def select_vhf(self, base, bias_tee=False):
        """Route the VHF path. `base` = your existing HF GPIO control word
        (ADC and other settings preserved); VHF_EN/bias are flipped on top."""
        word = (base | VHF_EN) & ~BIAS_HF
        if bias_tee:
            word |= BIAS_VHF
        self.gpio(word)
        return word

    def select_hf(self, base):
        word = (base | BIAS_HF) & ~VHF_EN & ~BIAS_VHF
        self.gpio(word)
        return word

    def r828d_probe(self):
        """Read the R828D ID register. Returns True if the tuner is reachable."""
        try:
            raw = self.i2c_read(R828D_ADDR, R828D_ID_REG, 1)[0]
        except usb.core.USBError as e:
            print(f"  I2C read failed (tuner not ACKing?): {e}")
            return False
        rev = _bitrev8(raw)
        ok = rev == R828D_ID_VAL
        print(f"  R828D reg0x00: raw=0x{raw:02X} bitrev=0x{rev:02X} "
              f"(expect 0x{R828D_ID_VAL:02X}) -> {'OK' if ok else 'mismatch'}")
        return ok  # a successful read at all already means the I2C path reaches it

    # --- STUBS: fill these from upstream / the bench ------------------------
    def si5351_set_clkb(self, ref_hz):
        """TODO: program Si5351 CLK2/CLKB to ref_hz (multisynth + PLL regs),
        the same way you already program CLK0 for the ADC, via self.i2c_write(
        SI5351_ADDR, reg, ...). For a first demo, hardcode the register set for
        one chosen reference frequency."""
        raise NotImplementedError("Si5351 CLKB programming — see vhf_host_bringup.md step 2")

    def r828d_init(self, ref_hz):
        """TODO: port init from steve-m/librtlsdr src/tuner_r82xx.c r82xx_init(),
        swapping its reg I/O for self.i2c_write/self.i2c_read(R828D_ADDR, ...).
        Set cfg->xtal = ref_hz (the Si5351 CLKB you programmed, NOT a crystal)."""
        raise NotImplementedError("R828D init — port r82xx_init() from librtlsdr")

    def r828d_set_freq(self, rf_hz):
        """TODO: port r82xx_set_freq(): set_mux (tracking filter) + set_pll
        (LO = rf_hz + IF, IF ~= 4.57 MHz with the 8 MHz filter) + lock check."""
        raise NotImplementedError("R828D tune — port r82xx_set_freq() from librtlsdr")


def main():
    ap = argparse.ArgumentParser(description="RX888 mk2 VHF front-end config")
    ap.add_argument("freq_hz", type=float, help="VHF RF frequency to tune, Hz")
    ap.add_argument("--ref", type=float, default=16e6,
                    help="R828D reference clock fed via Si5351 CLKB (Hz)")
    ap.add_argument("--bias-tee", action="store_true",
                    help="enable DC bias on the VHF antenna port")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0,
                    help="your existing HF GPIO control word (hex); the VHF/bias "
                         "bits are flipped on top so ADC/other settings persist")
    ap.add_argument("--hf", action="store_true", help="switch back to HF and exit")
    ap.add_argument("--probe-only", action="store_true",
                    help="select VHF, probe the tuner, then stop")
    args = ap.parse_args()

    rx = RX888()

    if args.hf:
        print(f"HF: GPIO word 0x{rx.select_hf(args.base):05X}")
        return

    print(f"VHF path: GPIO word 0x{rx.select_vhf(args.base, bias_tee=args.bias_tee):05X}")
    if not rx.r828d_probe():
        print("  tuner not reachable — check the VHF GPIO/power before tuning")
    if args.probe_only:
        return

    rx.si5351_set_clkb(int(args.ref))          # STUB
    rx.r828d_init(int(args.ref))               # STUB
    rx.r828d_set_freq(int(args.freq_hz))       # STUB
    print(f"tuned to {args.freq_hz/1e6:.4f} MHz; IF ~4.57 MHz — "
          f"now stream and FFT the IF")


if __name__ == "__main__":
    main()
