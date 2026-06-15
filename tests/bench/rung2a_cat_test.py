#!/usr/bin/env python3
"""tests/bench/rung2a_cat_test.py — Rung 2a: QDX CAT serial control test.

Proves that the bench host can set frequency, key/unkey PTT, and read
responses from the QDX over its USB Virtual COM Port using the Kenwood
TS-480/TS-440 CAT command subset.  No RX888, no RF — just the QDX
connected via USB.

The QDX firmware is the independent oracle: TQ; reads the QDX's actual
TX/RX state machine, not an echo of what we sent.

Env vars:
  QDX_PORT         serial port path (required, e.g. /dev/ttyACM0)
  QDX_BAUD         baud rate (default 9600; irrelevant for USB CDC)
  QDX_FREQ_OFFSET  Hz offset for the set-frequency test (default 1000)
"""

import os
import sys

# Allow importing qdx_cat from the same directory.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from qdx_cat import QdxCat, QdxCatError


def main():
    port = os.environ.get("QDX_PORT")
    if not port:
        print("RUNG2A CAT FAIL — QDX_PORT not set", file=sys.stderr)
        sys.exit(1)

    baud = int(os.environ.get("QDX_BAUD", "9600"))
    freq_offset = int(os.environ.get("QDX_FREQ_OFFSET", "1000"))

    checks = 0
    firmware = "?"

    try:
        with QdxCat(port, baudrate=baud) as qdx:
            # 1. Liveness — FA; query (proven on hardware)
            orig_freq = qdx.get_freq()
            print(f"RUNG2A: freq read -> {orig_freq} Hz")
            checks += 1

            # 2. Identity + firmware (informational, not gating)
            id_str = qdx.get_id()
            firmware = qdx.get_firmware()
            print(f"RUNG2A: ID -> {id_str}, firmware -> {firmware}")
            checks += 1

            # 3. Frequency set (+offset) and read-back
            target = orig_freq + freq_offset
            readback = qdx.set_freq(target)
            if readback != target:
                print(f"RUNG2A CAT FAIL — freq set: expected {target}, "
                      f"read back {readback}")
                sys.exit(1)
            print(f"RUNG2A: freq set {target} Hz, read-back matches OK")
            checks += 1

            # 4. Frequency restore
            restored = qdx.set_freq(orig_freq)
            if restored != orig_freq:
                print(f"RUNG2A CAT FAIL — freq restore: expected {orig_freq}, "
                      f"read back {restored}")
                sys.exit(1)
            print(f"RUNG2A: freq restore -> {restored} Hz OK")
            checks += 1

            # 5. PTT cycle: TX; -> TQ1 -> RX; -> TQ0
            pre = qdx.get_tx_state()
            if pre:
                print("RUNG2A CAT FAIL — QDX already in TX before PTT test")
                sys.exit(1)
            print("RUNG2A: TQ -> RX (pre-PTT) OK")
            checks += 1

            qdx.tx_on()
            tx_state = qdx.get_tx_state()
            if not tx_state:
                print("RUNG2A CAT FAIL — TQ not TX after TX;")
                sys.exit(1)
            print("RUNG2A: TX; -> TQ1 OK")

            qdx.tx_off()
            rx_state = qdx.get_tx_state()
            if rx_state:
                print("RUNG2A CAT FAIL — TQ not RX after RX;")
                sys.exit(1)
            print("RUNG2A: RX; -> TQ0 OK")
            checks += 1

            # 6. IF; cross-check — freq matches FA
            info = qdx.get_info()
            if info["freq"] != orig_freq:
                print(f"RUNG2A CAT FAIL — IF freq {info['freq']} != "
                      f"FA freq {orig_freq}")
                sys.exit(1)
            print(f"RUNG2A: IF; -> freq={info['freq']} "
                  f"mode={info['mode']} tx={'TX' if info['tx'] else 'RX'} OK")
            checks += 1

        # Verdict
        print(f"RUNG2A CAT OK ({checks} checks, fw {firmware})")

    except QdxCatError as exc:
        print(f"RUNG2A CAT FAIL — {exc}")
        sys.exit(1)
    except Exception as exc:
        print(f"RUNG2A CAT FAIL — unexpected: {exc}")
        sys.exit(1)


if __name__ == "__main__":
    main()
