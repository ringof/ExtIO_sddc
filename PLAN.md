# PLAN — Fix findings #2, #8, #9 from code review

Branch: `claude/review-fx3-firmware-Fw7Qz`

---

## Fix #2: Debug NUL termination clobbers last character

### Problem

In `USBHandler.c:304`, the `READINFODEBUG` handler does:

```c
glEp0Buffer[len-1] = 0;
```

`MyDebugSNPrint()` returns `len` = count of formatted characters **excluding**
the NUL.  So `glEp0Buffer[len-1]` overwrites the last printable character
of the debug message.

**Secondary issue:** `glBufDebug` is 100 bytes (`MAXLEN_D_USB`), but
`glEp0Buffer` is only 64 bytes (`CYFX_SDRAPP_MAX_EP0LEN`).  The uncapped
`memcpy(glEp0Buffer, glBufDebug, len)` can overflow `glEp0Buffer` when
`glDebTxtLen > 64`.

### Fix

In `SDDC_FX3/USBHandler.c`, `READINFODEBUG` handler (lines 297-313):

1. **Cap `len`** to `CYFX_SDRAPP_MAX_EP0LEN - 1` (63) inside the
   critical section, before the memcpy.  This prevents the EP0 buffer
   overflow and leaves room for the NUL.
2. **Change NUL placement** from `glEp0Buffer[len-1] = 0` to
   `glEp0Buffer[len] = '\0'`.
3. **Send `len + 1`** bytes via `CyU3PUsbSendEP0Data`.

This preserves the documented protocol contract ("last byte is NUL") while
transmitting all printable characters.

Truncation when `glDebTxtLen > 63`: acceptable for a best-effort debug
channel.  The buffer already drops messages on overflow.

### Files changed

- `SDDC_FX3/USBHandler.c` — `READINFODEBUG` case (~5 lines)

---

## Fix #8: Si5351 I2C writes ignore error status

### Problem

`SetupPLL()` and `SetupMultisynth()` in `Si5351.c` call `I2cTransfer()`
without checking the return value.  Likewise, `si5351aSetFrequencyA()` and
`si5351aSetFrequencyB()` call `I2cTransferW1()` multiple times at the end
(PLL reset, clock control) without checking status.

If the I2C bus wedges during ADC clock programming, the firmware silently
continues with a bad or missing clock — which is the exact scenario
described in `docs/wedge_detection.md`.

### Fix

1. **Change `SetupPLL` and `SetupMultisynth` return type** from `void` to
   `CyU3PReturnStatus_t`.  Return the result of their `I2cTransfer` call.

2. **In `si5351aSetFrequencyA` and `si5351aSetFrequencyB`**, capture
   return values from `SetupPLL`, `SetupMultisynth`, and the trailing
   `I2cTransferW1` calls.  On any failure, log via `DebugPrint` and
   return early.

3. **Change the return type** of `si5351aSetFrequencyA` and
   `si5351aSetFrequencyB` from `void` to `CyU3PReturnStatus_t`.
   Update the header `Si5351.h` accordingly.

4. **Update call sites** — `STARTADC` in `USBHandler.c` already has an
   `isHandled` guard; thread the error through so a failed clock setup
   returns `isHandled = CyFalse` (which sends a STALL to the host).
   `RunApplication.c` calls `si5351aSetFrequencyB` for hardware detection;
   add a status check + `DebugPrint` there.

### Files changed

- `SDDC_FX3/driver/Si5351.c` — return types + error checks (~20 lines)
- `SDDC_FX3/driver/Si5351.h` — updated signatures
- `SDDC_FX3/USBHandler.c` — `STARTADC` error propagation (~3 lines)
- `SDDC_FX3/RunApplication.c` — detection-time frequency call (~2 lines)

---

## Fix #9: AD8340 → AD8370 naming inconsistency

### Problem

The actual RX888mk2 hardware uses the **AD8370** VGA.  The source and one
doc file use `AD8340_VGA` — a typo carried forward from the original
ExtIO_sddc codebase.  `docs/architecture.md` and `rx888r2.c` already say
AD8370.

### Fix

Rename the symbol `AD8340_VGA` → `AD8370_VGA` in:

- `SDDC_FX3/protocol.h` — enum value
- `SDDC_FX3/USBHandler.c` — `SETARGFX3` switch case
- `SDDC_FX3/DebugConsole.c` — `SETARGFX3List[]` string literal
- `docs/LICENSE_ANALYSIS.md` — one prose reference

Wire value stays `11`; no protocol-level change.

### Files changed

- `SDDC_FX3/protocol.h`
- `SDDC_FX3/USBHandler.c`
- `SDDC_FX3/DebugConsole.c`
- `docs/LICENSE_ANALYSIS.md`

---

## Validation

- **Build:** `make clean && make all` in `SDDC_FX3/` (requires
  `arm-none-eabi-gcc`).  Produces `SDDC_FX3.img`.
- **#2 validation:** Use `fx3_cmd` or `rx888_stream` to read
  `READINFODEBUG` after enabling debug mode; verify last character of
  each message is no longer truncated.
- **#8 validation:** Code inspection — every `I2cTransfer` /
  `I2cTransferW1` call in Si5351.c now has its return value checked.
  Runtime: disconnect Si5351 or corrupt I2C bus; verify firmware logs the
  error and `STARTADC` STALLs EP0.
- **#9 validation:** `grep -r AD8340 SDDC_FX3/ docs/` returns zero hits.

## Regression

- Existing `tests/fw_test.sh` TAP suite covers TESTFX3, STARTADC,
  SETARGFX3, READINFODEBUG, GPIO, EP0 overflow, and streaming.
  All tests should continue to pass.
- Symbol rename (#9) is source-only; wire value `11` is unchanged, so
  host compatibility is unaffected.
