# SDDC_FX3 Test Tools

Host-side test tools for validating the SDDC_FX3 firmware on RX888mk2
hardware.

## Prerequisites

```
sudo apt install libusb-1.0-0-dev
git submodule update --init       # pulls rx888_tools for firmware upload
```

## Building

```
cd tests
make            # builds fx3_cmd and rx888_stream
make fx3_cmd    # builds just the vendor command exerciser
make clean      # removes build artifacts
```

## fx3_cmd -- Vendor Command Exerciser

Sends individual USB vendor requests to an RX888mk2 and reports
PASS/FAIL.  The device must already have firmware loaded (PID 0x00F1).

### Basic commands

```
./fx3_cmd test                          # probe device info
./fx3_cmd gpio 0x800                    # set GPIO word (LED_BLUE on)
./fx3_cmd adc 64000000                  # set ADC clock to 64 MHz
./fx3_cmd att 15                        # set attenuator (0-63)
./fx3_cmd vga 128                       # set VGA gain (0-255)
./fx3_cmd start                         # start GPIF streaming
./fx3_cmd stop                          # stop GPIF streaming
./fx3_cmd i2cr 0xC0 0 1                 # I2C read (Si5351 status)
./fx3_cmd i2cw 0xC0 3 0xFF              # I2C write
./fx3_cmd raw 0xCC                      # send arbitrary vendor request
./fx3_cmd stats                         # read GETSTATS counters
./fx3_cmd reset                         # reboot FX3 to bootloader
```

### Automated test commands

Each prints `PASS`/`FAIL` and exits 0/1:

```
./fx3_cmd ep0_overflow                  # wLength > 64 must STALL (#6)
./fx3_cmd oob_brequest                  # bRequest=0xCC bounds check (#21)
./fx3_cmd oob_setarg                    # SETARGFX3 wIndex=0xFFFF (#20)
./fx3_cmd console_fill                  # 35 chars to 32-byte buffer (#13)
./fx3_cmd debug_race                    # 50 rapid debug I/O cycles (#8)
./fx3_cmd debug_poll                    # debug console "?" command (#26)
./fx3_cmd pib_overflow                  # PIB error detection (#10)
./fx3_cmd stack_check                   # stack watermark > 25% (#12)
./fx3_cmd stats_i2c                     # I2C failure counter
./fx3_cmd stats_pib                     # PIB error counter
./fx3_cmd stats_pll                     # Si5351 PLL lock status
./fx3_cmd stop_gpif_state               # GPIF SM stops correctly
./fx3_cmd stop_start_cycle              # 5x STOP+START with data verify
./fx3_cmd pll_preflight                 # STARTFX3 rejected without clock
./fx3_cmd wedge_recovery                # DMA wedge + STOP+START recovery
```

### Interactive debug console

```
./fx3_cmd debug
```

Opens a live debug console over USB:

- Enables debug mode via `TESTFX3 wValue=1`
- Puts stdin in raw mode (character-at-a-time, no echo)
- Polls `READINFODEBUG` every 50 ms for firmware output
- Typed characters are sent to the firmware; Enter sends CR
- Ctrl-C to quit

#### Local command escape (`!`)

Type `!` to switch to local command mode.  This lets you run any
`fx3_cmd` subcommand without leaving the debug session:

```
!adc 64000000        → runs fx3_cmd adc 64000000
!start               → runs fx3_cmd start
!stop_gpif_state     → runs the GPIF state test
!help                → lists all available local commands
```

Debug output polling continues between commands.  Some test commands
take several seconds; output is paused during execution.

## fw_test.sh -- Automated TAP Test Suite

Runs the full test suite (30 tests, 33 with streaming) in TAP format.
Handles firmware upload automatically via `rx888_stream`.

```
./fw_test.sh --firmware ../SDDC_FX3/SDDC_FX3.img
```

### Options

```
--firmware PATH        Firmware .img file (required)
--stream-seconds N     Streaming capture duration (default: 5)
--rx888-stream PATH    Path to rx888_stream binary
--skip-stream          Skip streaming tests (tests 31-33)
--sample-rate HZ       ADC sample rate (default: 32000000)
```

### Requirements

- RX888mk2 connected via USB
- `fx3_cmd` and `rx888_stream` built (run `make` first)
- User must have USB device permissions (udev rule or run as root)

### Example output

```
1..30
ok 1 - firmware upload (PID 0x00F1)
ok 2 - device probe (hwconfig=0x04)
ok 3 - GPIO set LED_BLUE
...
ok 30 - GETSTATS PIB counter > 0
```

## File map

| File | Purpose |
|------|---------|
| `fx3_cmd.c` | Vendor command exerciser and test harness |
| `fw_test.sh` | TAP test suite wrapper |
| `Makefile` | Build system for test tools |
| `rx888_tools/` | Git submodule: firmware uploader and USB streamer |
