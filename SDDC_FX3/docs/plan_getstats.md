# Plan: GETSTATS Vendor Request

## Goal

Add a read-only EP0 vendor request (`GETSTATS`, `0xB3`) that returns a
snapshot of firmware-internal counters and state.  The implementation
touches only existing callback sites and already-allocated storage.
It does not modify the streaming data path, DMA configuration, or
callback notification modes.

## Scope constraints

- No new threads, DMA channels, or interrupt registrations.
- All counters use the existing `glCounter[20]` array
  (`DebugConsole.c:32`), which is already allocated and unused.
- 32-bit aligned loads/stores are atomic on the ARM926EJ-S core;
  no locking required for individual counter increments.
- Snapshot read across multiple counters is best-effort,
  which is acceptable for diagnostics.

---

## Response layout

The GETSTATS response is a fixed-size binary payload returned over EP0.

| Byte offset | Size | Field                  | Source                                          |
|-------------|------|------------------------|-------------------------------------------------|
| 0-3         | 4    | DMA buffer completions | `glDMACount` (already maintained)               |
| 4           | 1    | GPIF SM state          | `CyU3PGpifGetSMState()` sampled at read time    |
| 5-8         | 4    | PIB error count        | `glCounter[0]`, incremented in PibErrorCallback  |
| 9-10        | 2    | Last PIB error arg     | new `glLastPibArg`, saved alongside count        |
| 11-14       | 4    | I2C failure count      | `glCounter[1]`, incremented in I2cTransfer       |
| 15-18       | 4    | EP underrun count      | `glCounter[2]`, incremented in USBEventCallback  |

**Total: 19 bytes** (well within the 64-byte EP0 limit).

The layout is packed and uses little-endian byte order (native ARM).

---

## Changes by file

### 1. `protocol.h` — add command code

Add `GETSTATS = 0xB3` to the `FX3Command` enum.

**Lines changed: 1**

### 2. `DebugConsole.c` — command name table

Replace the `"0xB3"` placeholder in `FX3CommandName[]` with `"GETSTATS"`.

**Lines changed: 1**

### 3. `StartStopApplication.c` — PIB error counter

In `PibErrorCallback`, inside the existing `if (cbType == CYU3P_PIB_INTR_ERROR)` block,
add two lines:

```c
glCounter[0]++;
glLastPibArg = cbArg;
```

Declare `extern uint32_t glCounter[];` and add a file-scope `uint16_t glLastPibArg;`
(or declare it in a shared header).

**Lines added: ~4** (2 increments + 2 declarations)

### 4. `i2cmodule.c` — I2C failure counter

Before the `return status;` at the end of `I2cTransfer()` (line 75), add:

```c
if (status != CY_U3P_SUCCESS)
    glCounter[1]++;
```

Add `extern uint32_t glCounter[];` at top of file.

**Lines added: 3**

### 5. `USBHandler.c` — EP underrun counter + GETSTATS handler

**5a. EP underrun counter (1 line)**

In `USBEventCallback`, case `CY_U3P_USB_EVENT_EP_UNDERRUN` (line 362),
add `glCounter[2]++;` next to the existing `DebugPrint`.

**5b. GETSTATS vendor request handler (~12 lines)**

New `case GETSTATS:` in the vendor-request switch:

```c
case GETSTATS:
{
    uint8_t gpifState = 0xFF;
    CyU3PGpifGetSMState(&gpifState);
    uint16_t off = 0;
    memcpy(&glEp0Buffer[off], &glDMACount, 4);   off += 4;
    glEp0Buffer[off++] = gpifState;
    memcpy(&glEp0Buffer[off], &glCounter[0], 4);  off += 4;
    memcpy(&glEp0Buffer[off], &glLastPibArg, 2);  off += 2;
    memcpy(&glEp0Buffer[off], &glCounter[1], 4);  off += 4;
    memcpy(&glEp0Buffer[off], &glCounter[2], 4);  off += 4;
    CyU3PUsbSendEP0Data(off, glEp0Buffer);
    isHandled = CyTrue;
    break;
}
```

Add `extern uint32_t glCounter[];` and `extern uint16_t glLastPibArg;`
at top of file.

**Lines added: ~15** (handler + declarations)

### 6. `StartStopApplication.c` — counter reset on start

In `StartApplication()`, next to the existing `glDMACount = 0;` (line 97),
zero the counter slots:

```c
glCounter[0] = glCounter[1] = glCounter[2] = 0;
glLastPibArg = 0;
```

**Lines added: 1**

---

## What is NOT included (and why)

| Omitted item | Reason |
|---|---|
| PIB errors by individual code | Requires decode logic and variable-length bucketing; `lastPibArg` captures the most recent code for post-mortem |
| DMA error counting | Would require changing `dmaMultiConfig.notification` to include error types, touching the streaming data path |
| USB reset / disconnect / SET_CONF counts | These events are already queued to `glEventAvailable` and logged via `MsgParsing`; they have an existing path to the host through the debug channel |
| SPI / ADC direct state | ADC is across the GPIF boundary (no direct query); attenuator is bit-banged GPIO, not SPI |

Any of these can be added later by assigning the next `glCounter[]` slot.

---

## Line count summary

| File | Lines added |
|---|---|
| `protocol.h` | 1 |
| `DebugConsole.c` | 1 |
| `StartStopApplication.c` | ~5 |
| `i2cmodule.c` | 3 |
| `USBHandler.c` | ~16 |
| **Total** | **~26** |

---

## Build and validation

**Build:** Standard FX3 firmware build — no new source files, no new
dependencies.

**Validation:**
1. Issue `GETSTATS` (`0xB3`) via USB control transfer; verify 19 bytes
   returned with sane values (DMA count advancing, GPIF state matching
   expected value for idle/streaming).
2. Trigger a known I2C failure (e.g., address a nonexistent device);
   verify I2C failure count increments.
3. Stream data and confirm DMA count advances; stop streaming and
   confirm GPIF state returns to IDLE.

**Regression:** All existing `debugging.md` tests pass unchanged.
GETSTATS is read-only and does not alter any state.
