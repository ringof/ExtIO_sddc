"""tests/bench/qdx_cat.py — Reusable QDX CAT (Kenwood TS-480) serial control.

Provides QdxCat, a context-manager class that speaks the QDX's Kenwood TS-480/
TS-440 CAT subset over its USB Virtual COM Port.  Used by rung 2a and every
later QDX-touching rung in the HWIL bench ladder.

Protocol notes (manual_operation_1_08.pdf pp 35-40):
  - Commands are 2-char alpha + optional params, terminated by ';'.
  - Unrecognised commands return '?;'.
  - USB CDC — baud rate is irrelevant but pyserial requires one.
"""

import re
import sys
import serial


class QdxCatError(Exception):
    """Any CAT-level error (timeout, unexpected response, etc.)."""


class QdxCat:
    """Kenwood CAT interface to a QDX transceiver."""

    def __init__(self, port: str, baudrate: int = 9600, timeout: float = 2.0,
                 log=None):
        """Open *port* and prepare for CAT exchanges.

        *log* — callable(str) for protocol trace lines.  Defaults to
        ``print`` so every exchange is visible to the operator.
        """
        self._log = log or print
        self._ser = serial.Serial(port, baudrate=baudrate, timeout=timeout)
        self._ser.reset_input_buffer()

    # ------------------------------------------------------------------
    # Low-level
    # ------------------------------------------------------------------

    def send(self, cmd: str) -> str:
        """Send *cmd* (appends ``;`` if missing), return the response up to ``;``.

        Raises QdxCatError on timeout or ``?;`` (unknown command).
        """
        if not cmd.endswith(";"):
            cmd += ";"
        tx = cmd.encode("ascii")
        self._ser.reset_input_buffer()
        self._ser.write(tx)
        self._ser.flush()

        rx = self._read_until_semi()
        self._log(f"  TX {tx!r}  RX {rx!r}")

        resp = rx.decode("ascii", errors="replace")
        if resp == "?;":
            raise QdxCatError(f"QDX returned ?; for command {cmd!r}")
        return resp

    def _read_until_semi(self) -> bytes:
        """Read bytes until ``;`` or timeout."""
        buf = bytearray()
        while True:
            ch = self._ser.read(1)
            if not ch:
                raise QdxCatError(
                    f"timeout waiting for ';' (got {bytes(buf)!r} so far)")
            buf += ch
            if ch == b";":
                return bytes(buf)

    # ------------------------------------------------------------------
    # Typed helpers
    # ------------------------------------------------------------------

    def get_id(self) -> str:
        """Return the ID string, e.g. ``'020'``."""
        resp = self.send("ID;")           # -> 'ID020;'
        return resp[2:-1]                 # strip 'ID' and ';'

    def get_firmware(self) -> str:
        """Return firmware version string, e.g. ``'1_08'``."""
        resp = self.send("VN;")           # -> 'VN1_08;'
        return resp[2:-1]

    def get_freq(self) -> int:
        """Return current VFO-A frequency in Hz."""
        resp = self.send("FA;")           # -> 'FA00014074000;'
        return int(resp[2:-1])

    def set_freq(self, hz: int) -> int:
        """Set VFO-A to *hz* and return the read-back frequency."""
        padded = f"{hz:011d}"
        self.send(f"FA{padded};")
        return self.get_freq()

    def get_tx_state(self) -> bool:
        """Return True if QDX is transmitting."""
        resp = self.send("TQ;")           # -> 'TQ0;' or 'TQ1;'
        return resp[2:-1] == "1"

    def tx_on(self) -> None:
        """Key PTT (TX;)."""
        self.send("TX;")

    def tx_off(self) -> None:
        """Unkey PTT (RX;)."""
        self.send("RX;")

    def get_info(self) -> dict:
        """Parse the IF; response into a dict.

        Returned keys: freq (int Hz), tx (bool), mode_num (int),
        mode (str 'LSB'/'USB'/…), raw (str).
        """
        resp = self.send("IF;")           # -> 'IF00014074000     +00000..;'
        raw = resp[2:-1]                  # strip 'IF' and ';'

        freq = int(raw[0:11])
        # Fields after the 11-digit freq: 5 spaces, 5-digit RIT offset,
        # 1-char RIT on/off, 1-char XIT, 1-char mem bank, 2-char mem num,
        # 1-char TX state, 1-char mode, …
        tx_char = raw[28]                 # '0' or '1'
        mode_num = int(raw[29])           # 1=LSB, 2=USB, …
        mode_map = {1: "LSB", 2: "USB", 3: "CW", 4: "FM", 5: "AM",
                    6: "FSK", 7: "CW-R", 9: "FSK-R"}
        return {
            "freq": freq,
            "tx": tx_char == "1",
            "mode_num": mode_num,
            "mode": mode_map.get(mode_num, f"?{mode_num}"),
            "raw": raw,
        }

    # ------------------------------------------------------------------
    # Lifecycle
    # ------------------------------------------------------------------

    def close(self) -> None:
        """Close the serial port."""
        if self._ser and self._ser.is_open:
            self._ser.close()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        # Safety: always unkey PTT so a crash never leaves the QDX keyed.
        try:
            self.tx_off()
        except Exception:
            pass
        self.close()
        return False
