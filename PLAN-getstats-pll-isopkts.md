# Plan: Add PLL lock status to GETSTATS and fix `isoPkts` misuse

## Background

GETSTATS (`0xB3`) currently returns 19 bytes of diagnostic counters.
Two items need attention:

1. **PLL lock status** — the `si5351_status` field was explicitly
   deferred as "planned as next extension" (see `diagnostics_side_channel.md`
   line 80).  All infrastructure exists: the Si5351 I2C driver, the
   `I2cTransfer()` helper, and the register-0 bit layout are documented.

2. **`epCfg.isoPkts = 1`** — `StartStopApplication.c:99` sets the
   `isoPkts` field on a **bulk** endpoint (EP1 IN, `0x81`).  Per the
   SDK header (`cyu3usb.h:446`), `isoPkts` is "Number of packets per
   micro-frame for **ISO** endpoints", and non-zero values are only
   valid on endpoints 3 and 7 (`cyu3usb.h:929`).  On a bulk EP1 the
   field is ignored by the SDK, but the non-zero value is misleading
   and could mask a copy-paste bug.

---

## Changes

### Task 1 — Append `si5351_status` byte to GETSTATS response

**Files:** `SDDC_FX3/USBHandler.c`, `tests/fx3_cmd.c`,
`docs/diagnostics_side_channel.md`, `SDDC_FX3/docs/debugging.md`,
`docs/architecture.md`

**Firmware (`USBHandler.c`, case GETSTATS):**

After the last `memcpy` (offset 15–18, EP underrun count), add:

```c
uint8_t si_status = 0;
I2cTransfer(0x00, SI5351_ADDR, 1, &si_status, CyTrue);
glEp0Buffer[off++] = si_status;                          // [19]
```

Response grows from 19 → 20 bytes.  `CyU3PUsbSendEP0Data(off, …)`
already uses the running `off` variable, so no other handler change is
needed.

**Cost:** one I2C register-0 read (~50 µs) per GETSTATS poll.  No I2C
contention during streaming (attenuator/VGA use GPIO bit-bang, not I2C).

**Host test tool (`tests/fx3_cmd.c`):**

- Update `GETSTATS_LEN` from 19 → 20.
- Add `uint8_t si5351_status` to `struct fx3_stats`.
- In `read_stats()`: `s->si5351_status = buf[19];`
- In `do_stats()` printf: append `pll=0x%02X` field.
- Add `do_test_stats_pll()`:
  - Read stats, verify `si5351_status` bit 7 (`SYS_INIT`) is clear
    (device has completed initialization).
  - Verify PLL A locked: `!(si5351_status & 0x20)`.
  - Print `PASS`/`FAIL` accordingly.
- Register the new `stats_pll` subcommand.

**Docs:**

- `diagnostics_side_channel.md`: Move `si5351_status` from "Not yet
  implemented" table to the implemented-fields table.  Update byte
  count 19 → 20 throughout.
- `SDDC_FX3/docs/debugging.md` §5: Add offset 19 row to the wire
  format table, update total length.
- `docs/architecture.md`: Update GETSTATS data column from "19 B IN"
  to "20 B IN".

### Task 2 — Fix `epCfg.isoPkts = 1` on bulk endpoint

**File:** `SDDC_FX3/StartStopApplication.c:99`

Change:

```c
epCfg.isoPkts = 1;
```

to:

```c
epCfg.isoPkts = 0;   /* bulk endpoint — isoPkts is for ISO EPs only */
```

**Rationale:** The SDK ignores this field for bulk endpoints, so the
change is a no-op at runtime — but it removes a misleading value that
could confuse future readers or trigger an error if the endpoint type
were ever changed.  Setting it to 0 matches the `CyU3PMemSet` zero-fill
two lines earlier and makes the intent explicit.

### Task 3 — Add automated test to `fw_test.sh`

Add a new TAP test case that runs `fx3_cmd stats_pll` and verifies the
PLL-A-locked assertion passes.  Place it after the existing stats tests
(tests 22–25).

### Task 4 — Update comment block in `fx3_cmd.c`

Update the GETSTATS response layout comment (lines 795–802) to include
the new offset-19 field.

---

## What is NOT in scope

- No new vendor command (DIAGFX3 0xB7) — this plan extends the
  existing GETSTATS response by one byte.
- No timestamp, USB PHY/link error, or recovery-count fields.
- No host-side (ExtIO DLL) changes — only the test tool is updated.

---

## Build / test procedure

```bash
# Firmware
cd SDDC_FX3 && make clean && make all

# Host test tool
cd tests && make clean && make

# Automated tests (device must be connected)
./tests/fw_test.sh --firmware SDDC_FX3/SDDC_FX3.img --skip-stream
```

---

## Risk assessment

| Change | Risk | Mitigation |
|--------|------|------------|
| I2C read in GETSTATS handler | Adds ~50 µs to EP0 response | EP0 is polled at 2–10 Hz; 50 µs is negligible |
| I2C failure during read | `I2cTransfer` increments `glCounter[1]`; `si_status` stays 0 | 0x00 means "both PLLs locked, init done" — safe default |
| `isoPkts` change | SDK ignores field for bulk EPs | No runtime behavior change; verified by reading SDK source |
| Response length 19→20 | Old host code requesting 19 bytes still works | `libusb_control_transfer` with `wLength=19` gets 19 bytes; byte 20 is simply not transferred |
