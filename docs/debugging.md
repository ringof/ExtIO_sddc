# FX3 Firmware Debugging Infrastructure

## Architecture overview

There are two independent debug channels, selected at compile time by `_DEBUG_USB_` (defined in `protocol.h`):

| | **UART serial console** | **Debug-over-USB** |
|---|---|---|
| Active when | `_DEBUG_USB_` is **not** defined | `_DEBUG_USB_` **is** defined (current default) |
| `DebugPrint` maps to | `CyU3PDebugPrint` (SDK UART driver) | `DebugPrint2USB` (custom EP0 buffer) |
| Input path | `UartCallback` via DMA, char-at-a-time | `READINFODEBUG` EP0 handler, `wValue` carries one char |
| Output path | UART TX DMA to physical serial pin | `bufdebug[]` buffer, polled by host via `READINFODEBUG` |
| Command dispatch | `UartCallback` -> queue -> `MsgParsing` -> `ParseCommand` | `READINFODEBUG` handler -> queue -> `MsgParsing` -> `ParseCommand` |

Both paths feed characters into the same `ConsoleInBuffer[32]` and trigger the same `ParseCommand()` on carriage return. The `TraceSerial()` function (guarded by `#ifdef TRACESERIAL`) is an additional layer that logs every vendor request to whatever output channel is active.

---

## Path 1: UART serial console

### Initialization

`InitializeDebugConsole` (`DebugConsole.c:284`) configures UART at 115200 baud, 1 stop bit, DMA mode. The SDK `CyU3PDebugInit` is layered on top with preamble disabled (`CyU3PDebugPreamble(CyFalse)`). A `MANUAL_IN` DMA channel with a single 16-byte buffer receives typed characters, firing `UartCallback` on each byte.

### Input flow

```
Physical UART RX -> DMA PROD event -> UartCallback()
  -> echo char back to terminal
  -> accumulate into ConsoleInBuffer[] (forced lowercase via | 0x20)
  -> on CR (0x0d): post USER_COMMAND_AVAILABLE to EventAvailable queue
```

The main `ApplicationThread` loop polls the queue every 100 ms and calls `MsgParsing()` -> `ParseCommand()`.

### Console commands

| Command | What it does |
|---------|-------------|
| `?` or empty | Print help, show `glDMACount` |
| `threads` | Walk the ThreadX linked list, print every thread name |
| `stack` | Scan the application thread stack for the `0xEFEFEFEF` fill pattern, report free bytes |
| `gpif` | Query GPIF state machine state via `CyU3PGpifGetSMState` |
| `reset` | `CyU3PDeviceReset(CyFalse)` -- warm reset the FX3 |

### Known issues

1. **Currently unreachable for output.** `_DEBUG_USB_` is defined in `protocol.h`, so `DebugPrint` maps to `DebugPrint2USB`. All output calls go to the USB buffer, not the UART pins. The UART hardware is still initialized (and `UartCallback` still fires), so input works but output is invisible on the serial port. To use the UART path for output, `_DEBUG_USB_` must be undefined.

2. **~~`stack` command has an unbounded scan.~~** Fixed: `DebugConsole.c:113` now uses `while (*DataPtr == 0xEFEFEFEF) DataPtr++` to scan upward through intact fill pattern, correctly reporting free space.  The scan terminates at the first overwritten word (the stack high-water mark) rather than running past the allocation.

3. **Second counter is dead code in USB-debug mode.** `RunApplication.c:235-240` -- the `Seconds` counter and its `glDMACount`-based increment are inside `#ifndef _DEBUG_USB_`, so they are compiled out. With `_DEBUG_USB_` defined, there is no periodic throughput indication on the console.

---

## Path 2: Debug-over-USB (READINFODEBUG)

### Activation

Debug-over-USB is off by default. It is enabled by the host sending:

```
TESTFX3 (0xAC) with wValue=1
```

This sets `flagdebug = true` (`USBhandler.c:276`). All subsequent `DebugPrint2USB` calls then buffer output instead of returning immediately.

### Output flow

```
DebugPrint2USB()
  -> MyDebugSNPrint() formats into local buf[100]
  -> memcpy into bufdebug[] (append at debtxtlen offset)

Host polls READINFODEBUG (0xBA) with wValue=0:
  -> if debtxtlen > 0: memcpy bufdebug -> glEp0Buffer, send via EP0, reset debtxtlen
  -> if debtxtlen == 0: STALL (signals "nothing to read")
```

### Input flow (remote console)

The host can also send characters to the firmware console via `READINFODEBUG` with `wValue = character`:

```
READINFODEBUG with wValue='t' -> stores 't' in ConsoleInBuffer
READINFODEBUG with wValue='?' -> stores '?'
READINFODEBUG with wValue=0x0D -> posts USER_COMMAND_AVAILABLE to queue
```

This mirrors the UART input path exactly -- same `ConsoleInBuffer`, same `ParseCommand()`.

### Known issues

1. **Race condition on `bufdebug`/`debtxtlen`.** The writer (`DebugPrint2USB`, app thread context) and reader (`READINFODEBUG`, USB callback context) share `bufdebug[]` with no lock. The `volatile` on `debtxtlen` prevents compiler reordering but not torn reads/writes of the buffer contents mid-`memcpy`.

2. **Overflow backpressure is just a sleep.** `DebugConsole.c:245` -- when the buffer is full, `DebugPrint2USB` sleeps 100 ms then checks again. If it is still full, the message is silently dropped. There is no retry loop and no indication.

3. **READINFODEBUG null-terminates at `len-1`, truncating the last character.** `USBhandler.c:304`:
   ```c
   glEp0Buffer[len-1] = 0;
   ```
   `len` is the formatted text length. This overwrites the last character of the message with a null terminator, so every debug read loses one byte.

4. **STALL-as-"empty" is unconventional.** When there is no debug data (`debtxtlen == 0`), the handler stalls EP0 (`USBhandler.c:312`). This is not an error -- it signals "no data yet." But from the host's perspective, a STALL is an error condition. The host must treat STALL as "poll again later" rather than "fatal error," which is non-standard and can trigger warning messages in verbose USB logging.

5. **Single-character input protocol.** Each `READINFODEBUG` call can only carry one character (in `wValue`). Typing a 6-character command like `"stack\r"` requires 6 separate USB control transfers. It works, but it is slow.

6. **The `debtxtlen` reset-then-send sequence is not atomic.** At `USBhandler.c:301-305`:
   ```c
   uint16_t len = debtxtlen;
   memcpy(glEp0Buffer, bufdebug, len);
   debtxtlen=0;                    // reset AFTER copy
   glEp0Buffer[len-1] = 0;
   CyU3PUsbSendEP0Data (len, glEp0Buffer);
   ```
   If `DebugPrint2USB` appends to `bufdebug` between the `memcpy` and `debtxtlen=0`, that new data is lost. The reset should happen before the copy, or under a lock.

---

## TraceSerial (vendor request logger)

Guarded by `#ifdef TRACESERIAL` (defined in `Application.h`). Called at `USBhandler.c:325` after every vendor request is handled:

```c
TraceSerial(bRequest, (uint8_t *)&glEp0Buffer[0], wValue, wIndex);
```

It skips `READINFODEBUG` (to avoid recursive tracing of the debug poll itself), then logs the command name and relevant parameters. For `SETARGFX3` it prints the argument name and value; for `GPIOFX3` and `STARTADC` it prints the data payload; for others it prints the first two raw bytes.

### Known issues

1. **Out-of-bounds on `FX3CommandName[]` and `SETARGFX3List[]`.** If `bRequest - 0xAA` exceeds the array bounds, or if `wIndex` exceeds the `SETARGFX3List` bounds, these index operations read past the end of the arrays.

2. **Logs from stale `glEp0Buffer`.** For commands that do not call `GetEP0Data` (like the `default:` stall path), `pdata` points to `glEp0Buffer` which still contains data from the previous request. The trace output will show misleading values under the default formatting case.

3. **Runs inside the USB setup callback.** Each `DebugPrint` call inside `TraceSerial` goes through `DebugPrint2USB` -> `MyDebugSNPrint` -> `memcpy`. This is extra processing in the USB callback context. Not a problem at human typing speeds, but could add latency to back-to-back vendor requests (e.g., during rapid attenuator sweeps).

---

## TESTFX3 (device info / debug toggle)

`USBhandler.c:270-279` -- returns 4 bytes via EP0:

| Byte | Contents |
|------|----------|
| 0 | `HWconfig` (e.g., 0x04 = RX888r2) |
| 1 | `FWconfig` high byte |
| 2 | `FWconfig` low byte |
| 3 | `vendorRqtCnt` (rolling 8-bit counter of vendor requests) |

Side effect: `wValue == 1` enables debug mode (`flagdebug = true`); any other `wValue` disables it.

### Known issue

The debug enable/disable is a side effect of a query command. A host that sends `TESTFX3` with `wValue=0` to query device info will disable debug output as a side effect. There is no way to query device info without affecting the debug state.

---

## MsgParsing event dispatch

`RunApplication.c:MsgParsing` -- decodes the 32-bit queue event by label (top 8 bits):

| Label | Action |
|-------|--------|
| 0 | Print USB event name from `EventName[]` |
| 1 | Print vendor request bytes (not currently posted by any code) |
| 2 | Print PIB error code (not currently posted by any code) |
| 0x0A (`USER_COMMAND_AVAILABLE`) | Call `ParseCommand()` |

### Known issues

1. **`EventName[]` has no bounds check.** `RunApplication.c:129` indexes `EventName[(uint8_t)qevent]`. The array has 23 entries (indices 0-22). USB events are SDK-defined `CyU3PUsbEventType_t` values. If the SDK delivers an event type >= 23, this is an out-of-bounds read. In practice the SDK values map to 0-22, but there is no guard.

2. **Labels 1 and 2 are dead code.** Nothing in the firmware posts events with label=1 or label=2.

---

## Summary of issues

| Area | Issue | Severity |
|------|-------|----------|
| USB debug buffer | Race condition on `bufdebug`/`debtxtlen` (no locking) | High |
| USB debug buffer | Non-atomic reset-then-send loses appended data | High |
| `READINFODEBUG` | Last byte truncated by null terminator at `len-1` | Low |
| `READINFODEBUG` | EP0 STALL used to signal "no data" (non-standard) | Low |
| `READINFODEBUG` | Single-character input requires many control transfers | Low |
| `DebugPrint2USB` | Buffer-full messages silently dropped after one sleep | Medium |
| `TESTFX3` | Debug enable/disable is side effect of device info query | Medium |
| `TraceSerial` | Out-of-bounds on `FX3CommandName[]` / `SETARGFX3List[]` | High |
| `TraceSerial` | Logs stale `glEp0Buffer` data for some command paths | Low |
| `TraceSerial` | Runs inside USB setup callback, adds latency | Low |
| `ParseCommand` | `stack` command has unbounded memory scan | Medium |
| `MsgParsing` | `EventName[]` indexed without bounds check | Medium |
| `MsgParsing` | Labels 1 and 2 are dead code | Low |
| UART output | Unreachable when `_DEBUG_USB_` is defined (current default) | Low |
