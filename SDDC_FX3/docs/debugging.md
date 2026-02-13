# FX3 Firmware Debug Reference

The SDDC_FX3 firmware provides two debug channels, an interactive
console, and a real-time vendor-request trace.  All are available on
the RX888mk2 without hardware modifications.

---

## 1. Debug Channels

There are two independent output paths.  Only one is active at a time,
selected at compile time.

### 1.1 Compile-Time Selection

Controlled by `_DEBUG_USB_` in `protocol.h`:

| `_DEBUG_USB_` | `DebugPrint()` expands to | Output path |
|---------------|---------------------------|-------------|
| Defined (default) | `DebugPrint2USB()` | USB EP0 via `READINFODEBUG` |
| Not defined | `CyU3PDebugPrint()` | UART TX at 115200 baud |

Console input works on both paths: UART RX callback or host-sent
characters in the `READINFODEBUG` vendor request `wValue` field.

### 1.2 UART Debug Console

#### Hardware

- UART TX/RX pins are active whenever `InitializeDebugConsole()` runs
  (always -- called early in `CyFxApplicationDefine`)
- 115200 baud, 8N1, no flow control
- Directly accessible via the UART test points on the RX888mk2 PCB

#### Setup

UART is initialized in `DebugConsole.c:InitializeDebugConsole()`:

1. `CyU3PUartInit()` -- start UART driver
2. Configure 115200 baud, TX+RX enabled, DMA mode
3. `CyU3PDebugInit()` -- attach SDK debug driver to UART
4. Create DMA channel for UART RX, single-byte transfers
5. `UartCallback()` fires on each received byte

#### Usage

Connect a 3.3V UART adapter to the UART test points.  Any terminal at
115200/8N1 will show debug output and accept console commands.

### 1.3 USB Debug-over-EP0

This is the default and most practical channel -- no extra hardware
needed.

#### Protocol

Uses vendor request `READINFODEBUG` (`0xBA`) on the default control
endpoint (EP0).  Each control transfer is bidirectional:

**Host-to-device (input character):**

| `wValue` | Meaning |
|-----------|---------|
| `0x00` | No input character -- just polling for output |
| `0x0D` | Carriage return -- triggers command execution |
| Any other | ASCII character accumulated into `glConsoleInBuffer[32]` (lowercased) |

Characters are accumulated one per control transfer.  The 32-byte
input buffer silently stops accepting characters when full; sending CR
flushes and executes the command.

**Device-to-host (debug output):**

| Condition | Data phase |
|-----------|-----------|
| `glDebTxtLen > 0` | Firmware copies `glBufDebug[]` (up to 100 bytes of ASCII) to the data phase, resets `glDebTxtLen` to 0 |
| `glDebTxtLen == 0` | Firmware STALLs EP0 to signal "no data available" |

The last byte of the data phase is a NUL terminator placed by the
firmware.  Usable text length is `actual_transfer_length - 1`.

#### Activation

USB debug output requires two conditions:

1. `glIsApplnActive == CyTrue` (device is enumerated and configured)
2. `glFlagDebug == true`

**`glFlagDebug` starts as `false`.**  The host enables it by sending:

```
TESTFX3 (0xAC) with wValue=1
```

This returns the normal 4-byte device info AND sets `glFlagDebug = true`
as a side effect.  Sending `TESTFX3` with `wValue=0` disables debug
mode again.

Until debug mode is enabled, all `DebugPrint()` calls are silently
discarded.  This prevents buffer overflow during early init when no
host is polling.

#### Output Buffer

| Item | Detail |
|------|--------|
| Buffer | `glBufDebug[100]` in `DebugConsole.c` (size = `MAXLEN_D_USB` from `protocol.h`) |
| Fill counter | `glDebTxtLen` (declared `volatile uint16_t`) |
| Formatter | `MyDebugSNPrint()` -- simplified printf supporting `%d`, `%x`, `%s`, `%u`, `%c` |
| Overflow | If `glDebTxtLen + len > MAXLEN_D_USB`, sleeps 100ms and retries once.  If still full, the message is dropped. |
| Synchronization | Writer (`DebugPrint2USB`, application thread) and reader (`READINFODEBUG` handler, USB callback context) use `CyU3PVicDisableAllInterrupts()`/`CyU3PVicEnableInterrupts()` to protect buffer access |

---

## 2. Interactive Console Commands

Available via both UART and USB debug input.  Commands are
case-insensitive (all input is lowercased).  Enter/CR executes.

| Command | Action | Example output |
|---------|--------|----------------|
| `?` or empty | Print help and current DMA transfer count | `Enter commands: threads, stack, gpif, reset; DMAcnt = 1a3f` |
| `threads` | List all ThreadX RTOS threads by name, walking the thread linked list | `This : '03:AppThread'` / `Found: '01:IdleThread'` |
| `stack` | Show free stack space in the application thread (compares against `0xEFEFEFEF` fill pattern) | `Stack free in AppThread is 1248/2048` |
| `gpif` | Query and display current GPIF state machine state number | `GPIF State = 3` |
| `reset` | Software reset the FX3 (returns to bootloader) | `RESETTING CPU` (then device re-enumerates) |
| anything else | Echo back the unrecognized input | `Input: 'foo'` |

### Typical Console Session (via USB)

```
$ fx3_cmd debug
debug: enabled (hwconfig=0x04 fw=2.2)
debug: polling for output, type commands + Enter (Ctrl-C to quit)
TESTFX3
?
Enter commands:
threads, stack, gpif, reset;
DMAcnt = 0

threads
threads:
This : '03:AppThread'
Found: '00:CyIdleThread'
Found: '01:CyHelperThread'

stack
stack:
Stack free in AppThread is 1312/2048

gpif
GPIF State = 0
```

---

## 3. TraceSerial -- Vendor Request Logger

When `TRACESERIAL` is defined in `Application.h` (default), the
`TraceSerial()` function in `USBHandler.c` logs every EP0 vendor
request to the debug output after it is processed.

### What Gets Logged

| Request | Format | Example |
|---------|--------|---------|
| `SETARGFX3` | `SETARGFX3  <param_name>  <value>` | `SETARGFX3  DAT31_ATT  15` |
| `GPIOFX3` | `GPIOFX3  0x<word>` | `GPIOFX3  0x800` |
| `STARTADC` | `STARTADC  <freq>` | `STARTADC  64000000` |
| `STARTFX3` / `STOPFX3` / `RESETFX3` | Command name only | `STOPFX3` |
| Known commands (others) | `<name>  0x<byte0>  0x<byte1>` | `TESTFX3  0x4  0x2` |
| Unknown bRequest | `0x<code>  0x<byte0>  0x<byte1>` | `0xcc  0x0  0x0` |
| `READINFODEBUG` | Suppressed (avoids infinite recursion) | -- |

### SETARGFX3 Parameter Names

The `SETARGFX3List[]` maps `wIndex` to human-readable names:

| wIndex | Name |
|--------|------|
| 0--9 | `"0"` through `"9"` (unused/reserved) |
| 10 | `DAT31_ATT` (DAT-31 attenuator, 0--63) |
| 11 | `AD8370_VGA` (AD8370 VGA gain, 0--255) |
| 12 | `PRESELECTOR` |
| 13 | `VHF_ATTENUATOR` |
| >= 14 | Out of bounds -- logged as `<index>  <value>` |

### Bounds Checking

TraceSerial validates array indices before use:

- `bRequest` is checked against `FX3_CMD_BASE` (0xAA) through
  `FX3_CMD_BASE + FX3_CMD_COUNT` (0xBB).  Out-of-range requests are
  logged as hex (`0x<code>`) instead of indexing `FX3CommandName[]`.
- `SETARGFX3` `wIndex` is checked against `SETARGFX3_LIST_COUNT` (14).
  Out-of-range indices are logged numerically instead of indexing
  `SETARGFX3List[]`.

---

## 4. Error Logging Helpers

`Support.c` provides two helpers used throughout the firmware:

| Function | Behavior |
|----------|----------|
| `CheckStatus(name, status)` | On success: prints `"<name> = Successful"`.  On failure: prints error name and calls `ErrorBlinkAndReset()` (blinks LED, then resets FX3). |
| `CheckStatusSilent(name, status)` | On success: silent.  On failure: same as `CheckStatus`. |

Both are gated by `glDebugTxEnabled` -- they are no-ops until the
UART/debug subsystem is initialized.

---

## 5. Host-Side Tools

### 5.1 `fx3_cmd` -- Vendor Command Exerciser

Built from `tests/fx3_cmd.c`.  Provides both interactive and automated
access to all debug facilities.

```
cd tests && make
./fx3_cmd <command> [args...]
```

#### Interactive Debug Console

```
./fx3_cmd debug
```

- Sends `TESTFX3 wValue=1` to enable debug mode
- Puts stdin in raw mode (character-at-a-time, no echo)
- Polls `READINFODEBUG` every 50ms
- Typed characters are sent in `wValue`; Enter sends CR
- Ctrl-C to quit

#### Automated Test Commands

These are non-interactive subcommands for use in scripts and CI.  Each
prints `PASS`/`FAIL` and exits 0/1.

| Command | Tests | Issue |
|---------|-------|-------|
| `fx3_cmd test` | Device probe -- reads hwconfig, firmware version | -- |
| `fx3_cmd gpio <bits>` | Set GPIO control word | -- |
| `fx3_cmd adc <freq>` | Set ADC clock frequency | -- |
| `fx3_cmd att <0-63>` | Set DAT-31 attenuator | -- |
| `fx3_cmd vga <0-255>` | Set AD8370 VGA gain | -- |
| `fx3_cmd start` / `stop` | Start/stop GPIF streaming | -- |
| `fx3_cmd i2cr <addr> <reg> <len>` | I2C read | -- |
| `fx3_cmd i2cw <addr> <reg> <byte>...` | I2C write | -- |
| `fx3_cmd raw <code>` | Send arbitrary vendor request (hex bRequest) | -- |
| `fx3_cmd ep0_overflow` | Send wLength > 64 -- must STALL | #6 |
| `fx3_cmd oob_brequest` | Send bRequest=0xCC (outside FX3CommandName bounds) | #21 |
| `fx3_cmd oob_setarg` | Send SETARGFX3 wIndex=0xFFFF (outside SETARGFX3List bounds) | #20 |
| `fx3_cmd console_fill` | Send 35 chars to 32-byte glConsoleInBuffer | #13 |
| `fx3_cmd debug_race` | 50 rapid interleaved SETARGFX3 + READINFODEBUG polls | #8 |
| `fx3_cmd debug_poll` | Send `?` + CR, verify help text comes back | #26 |
| `fx3_cmd pib_overflow` | Start streaming without reading EP1, verify PIB error in debug output | #10 |
| `fx3_cmd stack_check` | Send `stack` command, parse watermark, verify > 25% free | #12 |
| `fx3_cmd reset` | Reboot FX3 to bootloader | -- |

### 5.2 `fw_test.sh` -- Automated TAP Test Suite

```
cd tests && make
./fw_test.sh --firmware ../SDDC_FX3/SDDC_FX3.img
```

Runs 22 tests (25 with streaming) in TAP format:

| # | Test | What it verifies |
|---|------|-----------------|
| 1 | Firmware upload | Device enumerates at PID 0x00F1 |
| 2 | Device probe | hwconfig = 0x04 (RX888r2) |
| 3 | GPIO | Set LED_BLUE |
| 4 | ADC clock | Set sample rate |
| 5--6 | Attenuator | Min (0) and max (63) |
| 7--8 | VGA | Min (0) and max (255) |
| 9 | Stop | Clean state |
| 10--12 | Stale tuner commands | 0xB4, 0xB5, 0xB8 all STALL (R82xx removed) |
| 13 | I2C NACK | Read from absent address 0xC2 correctly fails |
| 14 | ADC clock-off | STARTADC freq=0 exercises I2cTransferW1 path |
| 15 | EP0 overflow | wLength > 64 STALLs (issue #6) |
| 16 | OOB bRequest | bRequest=0xCC survives (issue #21) |
| 17 | OOB SETARGFX3 wIndex | wIndex=0xFFFF survives (issue #20) |
| 18 | Console buffer fill | 35-char input survives (issue #13) |
| 19 | Debug buffer race | 50 rapid poll cycles survive (issue #8) |
| 20 | Debug console over USB | `?` command returns help text (issue #26) |
| 21 | PIB overflow | GPIF overflow produces "PIB error" in debug output (issue #10) |
| 22 | Stack watermark | Free > 25% of 2048 bytes after init (issue #12) |
| 23--25 | Streaming (optional) | Data capture, byte count, non-zero data |

Options:

```
--firmware PATH        Firmware .img file (required)
--stream-seconds N     Streaming duration (default: 5)
--rx888-stream PATH    Path to rx888_stream binary
--skip-stream          Skip streaming tests (tests 23--25)
--sample-rate HZ       ADC sample rate (default: 32000000)
```

---

## 6. File Map

| File | Debug role |
|------|-----------|
| `DebugConsole.c` | UART init, USB debug buffer, printf formatter, console command parser, `ConsoleAccumulateChar()` |
| `USBHandler.c` | `READINFODEBUG` EP0 handler, `TraceSerial()` vendor request logger |
| `Support.c` | `CheckStatus()` / `CheckStatusSilent()` -- error logging with LED blink on failure |
| `protocol.h` | `_DEBUG_USB_` / `MAXLEN_D_USB` compile-time selection, `READINFODEBUG` command code |
| `Application.h` | `DebugPrint` macro routing (`DebugPrint2USB` vs `CyU3PDebugPrint`), `TRACESERIAL` define |
| `tests/fx3_cmd.c` | Host-side vendor command tool with interactive debug console |
| `tests/fw_test.sh` | Automated TAP test suite |
