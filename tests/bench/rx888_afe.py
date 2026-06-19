"""tests/bench/rx888_afe.py — Minimal EP0 driver for RX888mk2 HF AFE controls.

Thin pyusb module for controlling the RX888mk2 HF analog front-end via USB
EP0 vendor commands.  Covers the PE4304 attenuator, AD8370 VGA, and GPIO-
controlled HF/VHF antenna switch.

Coexists with radiod (which uses EP1 bulk streaming) — no USB interface
claim is needed for EP0 control transfers.

Protocol reference: SDDC_FX3/protocol.h
Pattern reference: vhf/rx888_vhf.py (RX888 class, on the VHF branch)

Requires: pyusb  (pip install pyusb)
"""

import struct

import usb.core
import usb.util


# ── Device identity ────────────────────────────────────────────────────
RX888_VID = 0x04B4
RX888_PID = 0x00F1          # application firmware

# ── EP0 vendor request codes (protocol.h enum FX3Command) ─────────────
TESTFX3    = 0xAC            # IN  -> [hwconfig, fw_hi, fw_lo, vendor_rqt_count]
GPIOFX3    = 0xAD            # OUT <- 4-byte LE GPIO control word
SETARGFX3  = 0xB6            # OUT <- wIndex=param, wValue=value
GETSTATS   = 0xB3            # IN  -> diagnostics; gpio_state at [26:30]

# bmRequestType constants
BM_OUT = 0x40                # host->device | vendor | device
BM_IN  = 0xC0                # device->host | vendor | device

# ── SETARGFX3 wIndex parameter indices (protocol.h enum ArgumentList) ─
DAT31_ATT  = 10              # PE4304 attenuator: 0–63 (0.5 dB steps)
AD8370_VGA = 11              # AD8370 VGA: 0–255 (two gain ranges)

# ── GPIO bit masks (protocol.h enum GPIOPin) ──────────────────────────
BIAS_HF  = 1 << 8            # HF antenna port bias-tee
BIAS_VHF = 1 << 9            # VHF antenna port bias-tee
VHF_EN   = 1 << 15           # HF/VHF antenna switch (set = VHF path)


class RX888AFEError(Exception):
    """Any AFE control error."""


class RX888AFE:
    """EP0 control of RX888mk2 HF analog front-end.

    Coexists with radiod (EP1 streaming) — no interface claim needed.
    """

    def __init__(self, dev=None):
        """Find or accept a pyusb device handle.

        If *dev* is None, searches for the first RX888mk2 on USB.
        Raises RX888AFEError if not found.
        """
        if dev is not None:
            self.dev = dev
        else:
            self.dev = usb.core.find(idVendor=RX888_VID, idProduct=RX888_PID)
            if self.dev is None:
                raise RX888AFEError(
                    "RX888mk2 not found on USB (04B4:00F1). "
                    "Powered up? Firmware loaded?"
                )

    # ── EP0 helpers ───────────────────────────────────────────────────────

    def _setarg(self, index, value):
        """Send a SETARGFX3 command: wIndex=parameter, wValue=value.

        SETARGFX3 is an OUT transfer with a data phase — the firmware calls
        CyU3PUsbGetEP0Data, so we must provide a (possibly empty) data payload.
        Returns the number of bytes transferred (should be 1).
        """
        ret = self.dev.ctrl_transfer(BM_OUT, SETARGFX3, value, index, b'\x00')
        return ret

    # ── Attenuator (PE4304) ───────────────────────────────────────────────

    def set_att(self, step):
        """Set the PE4304 attenuator.

        *step* is 0–63, in 0.5 dB increments (0 = 0 dB, 63 = 31.5 dB).
        """
        if not 0 <= step <= 63:
            raise RX888AFEError(f"att step {step} out of range 0–63")
        self._setarg(DAT31_ATT, step)

    # ── VGA (AD8370) ──────────────────────────────────────────────────────

    def set_vga(self, code):
        """Set the AD8370 VGA gain code.

        *code* is 0–255.  Two gain ranges:
          - 0–127: low-gain mode (increasing)
          - 128–255: high-gain mode (increasing, ~17 dB offset)
        """
        if not 0 <= code <= 255:
            raise RX888AFEError(f"VGA code {code} out of range 0–255")
        self._setarg(AD8370_VGA, code)

    # ── GPIO ──────────────────────────────────────────────────────────────

    def gpio(self, word):
        """Write a 32-bit GPIO control word (GPIOFX3)."""
        self.dev.ctrl_transfer(BM_OUT, GPIOFX3, 0, 0,
                               struct.pack("<I", word & 0xFFFFFFFF))

    def read_gpio(self):
        """Read the live GPIO state from GETSTATS bytes [26:30].

        Returns the 32-bit GPIO word, or None if the firmware payload is
        too short (pre-v2.0 firmware).
        """
        buf = bytes(self.dev.ctrl_transfer(BM_IN, GETSTATS, 0, 0, 64))
        if len(buf) >= 30:
            return int.from_bytes(buf[26:30], "little")
        return None

    # ── Liveness ──────────────────────────────────────────────────────────

    def check_alive(self):
        """Confirm the firmware answers TESTFX3.

        Returns (hwconfig, fw_major, fw_minor).
        Raises RX888AFEError if the device doesn't respond.
        """
        try:
            data = bytes(self.dev.ctrl_transfer(BM_IN, TESTFX3, 0, 0, 4))
        except usb.core.USBError as e:
            raise RX888AFEError(
                f"RX888 on USB but firmware not answering TESTFX3: {e}"
            )
        hw, fw_hi, fw_lo, _rqt = data[0], data[1], data[2], data[3]
        return hw, fw_hi, fw_lo

    def vendor_rqt_count(self):
        """Read the vendor request counter from TESTFX3 byte 3.

        This counter increments each time the firmware processes a
        SETARGFX3 command (ATT, VGA, or watchdog).  Useful for verifying
        that EP0 commands are reaching the firmware.
        Returns the count (0–255, wraps).
        """
        data = bytes(self.dev.ctrl_transfer(BM_IN, TESTFX3, 0, 0, 4))
        return data[3]

    def getstats_raw(self, length=64):
        """Read the full GETSTATS diagnostic payload (up to *length* bytes).

        Returns raw bytes — caller interprets.
        """
        return bytes(self.dev.ctrl_transfer(BM_IN, GETSTATS, 0, 0, length))
