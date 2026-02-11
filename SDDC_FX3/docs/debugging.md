# FX3 Firmware Debug Infrastructure

The firmware provides two independent debug channels: UART serial and
USB debug-over-EP0. Both share the same interactive console and
`DebugPrint()` output, selected at compile time.

## Compile-Time Selection

Controlled by `_DEBUG_USB_` in `protocol.h`:

| `_DEBUG_USB_` | `DebugPrint` expands to | Output path |
|---------------|------------------------|-------------|
| Defined (default) | `DebugPrint2USB()` | USB EP0 via `READINFODEBUG` |
| Not defined | `CyU3PDebugPrint()` | UART TX at 115200 baud |

Console input works on both paths: UART RX callback or host-sent
characters in the `READINFODEBUG` vendor request `wValue` field.

## UART Debug

### Hardware
- UART TX/RX pins are active whenever `InitializeDebugConsole()` runs
  (always — it's called early in `CyFxApplicationDefine`)
- 115200 baud, 8N1, no flow control
- Directly accessible via the UART test points on the RX888mk2 PCB

### Setup
UART is initialized in `DebugConsole.c:InitializeDebugConsole()`:
1. `CyU3PUartInit()` — start UART driver
2. Configure 115200 baud, TX+RX enabled, DMA mode
3. `CyU3PDebugInit()` — attach SDK debug driver to UART (enables
   `CyU3PDebugPrint`)
4. Create DMA channel for UART RX, single-byte transfers
5. `UartCallback()` fires on each received byte

### Usage
Connect a 3.3V UART adapter to the UART test points. Any terminal at
115200/8N1 will show debug output and accept console commands.

## USB Debug-over-EP0

### Protocol
Uses vendor request `READINFODEBUG` (0xBA) on the default control
endpoint.  The protocol is bidirectional within a single control
transfer:

**Upstream (host to device):**
- A single ASCII character is carried in the `wValue` field of the
  SETUP packet
- `wValue = 0` means no input character (just polling for output)
- `wValue = 0x0D` (CR) triggers command execution
- Characters are accumulated in `ConsoleInBuffer[32]`, lowercased

**Downstream (device to host):**
- If `debtxtlen > 0`: firmware copies `bufdebug[]` to EP0 data phase
  (up to 100 bytes of ASCII), resets `debtxtlen` to 0
- If `debtxtlen == 0`: firmware stalls EP0 to signal "no data"

### Activation
USB debug output requires two conditions:
1. `glIsApplnActive == CyTrue` (device is enumerated and configured)
2. `flagdebug == true` (set when host sends `TESTFX3` with `wValue=1`)

Until the host sends `TESTFX3` with `wValue=1`, `DebugPrint2USB()` is
a no-op — messages are silently discarded. This avoids buffer overflow
during early init when no host is polling.

### Host Implementation
A minimal host-side poller is straightforward:

```c
// libusb example: poll FX3 debug output
uint8_t buf[100];
int len;

// Enable debug mode
libusb_control_transfer(dev, 0xC0, TESTFX3, 1, 0, buf, 4, 1000);

// Poll loop
while (1) {
    buf[0] = 0;  // no input character
    len = libusb_control_transfer(dev,
        0xC0,            // bmRequestType: vendor, device-to-host
        READINFODEBUG,   // bRequest: 0xBA
        0,               // wValue: input char (0 = none)
        0,               // wIndex
        buf, 100,        // data phase buffer
        100);            // timeout ms
    if (len > 0) {
        buf[len] = 0;
        printf("%s", buf);
    }
    usleep(50000);  // 50ms poll interval
}
```

To send a console command, set `wValue` to each character in sequence,
then send `wValue = 0x0D` to execute.

### Buffer Details
- `bufdebug[100]` — accumulation buffer in `DebugConsole.c`
- `debtxtlen` — current fill level (declared `volatile`)
- `DebugPrint2USB()` formats the message via `MyDebugSNPrint()` (a
  simplified printf supporting `%d`, `%x`, `%s`, `%u`, `%c`), then
  appends to `bufdebug`
- If the buffer would overflow, `DebugPrint2USB()` sleeps 100ms and
  retries once. If still full, the message is dropped.
- **No synchronization** between the writer (`DebugPrint2USB`, runs in
  application thread) and the reader (`READINFODEBUG` handler, runs in
  USB callback context). This is a known race condition — see review
  issues.

## Interactive Console Commands

Available via both UART and USB debug input:

| Command | Action |
|---------|--------|
| `?` or empty | Print help and current DMA transfer count |
| `threads` | List all ThreadX RTOS threads by name |
| `stack` | Show free stack space in the application thread |
| `gpif` | Query and print current GPIF state machine state |
| `reset` | Software reset the FX3 (returns to bootloader) |

Commands are case-insensitive (input is lowercased). Enter/CR
executes.

## Trace Output

When `TRACESERIAL` is defined (default), the `TraceSerial()` function
in `USBhandler.c` logs every vendor request to the debug output:

```
GPIOFX3     0x1a0
STARTADC    64000000
TUNERINIT   28800000
TUNERTUNE   100000000
SETARGFX3   R82XX_ATTENUATOR    5
```

This provides a real-time trace of all host commands without any
host-side instrumentation.

## File Map

| File | Debug role |
|------|-----------|
| `DebugConsole.c` | UART init, USB debug buffer, printf formatter, console parser |
| `USBhandler.c` | `READINFODEBUG` EP0 handler, `TraceSerial()`, `r820_initialize()` |
| `Support.c` | `CheckStatus()` / `CheckStatusSilent()` — error logging helpers |
| `protocol.h` | `_DEBUG_USB_` / `MAXLEN_D_USB` compile-time selection |
| `Application.h` | `DebugPrint` macro selection |
