# FX3 Stop/Start Debug Analysis — State of Investigation

## Failing Tests
- **Test 30 (hw_smoke)**: 0 bytes received after STARTFX3
- **Test 32 (stop_start_cycle)**: 0 bytes on cycle 1/5, DMA_count=8, GPIF_state=9 (TH0_WAIT), PIB_err=4

## Key Finding: Firmware is NOT the problem

All diagnostic tests confirm:
1. **PLL is locked** — `pll=0x01` is normal (REVID=1, all status bits clear; SYS_INIT is bit 7, not bit 0)
2. **GPIF SM starts** — `GO s=2` (advances to TH0_RD)
3. **DMA produces data** — `DMA=8` (128KB produced) before stalling at TH0_WAIT (waiting for USB drain)
4. **Nobody drains EP1** → PIB overflow → watchdog fires → recovery fails (rc=68)
5. **gpio_extremes/SHDWN is irrelevant** — fails identically without it (Test D)
6. **Settle time is irrelevant** — fails at 0.1s through 2.0s (Test F)
7. **rx888_stream works perfectly** — 60 MiB/s streaming (Test C)

## Root Cause: fx3_cmd's bulk reads don't receive data from EP1

The FX3 sends data, but `libusb_bulk_transfer()` in fx3_cmd never receives it.
Meanwhile `rx888_stream` (async transfers) works fine on the same device.

## USB Setup Comparison

### fx3_cmd (tests/fx3_cmd.c)
```
open_rx888():
  libusb_open_device_with_vid_pid()
  libusb_detach_kernel_driver(h, 0)
  libusb_claim_interface(h, 0)
  libusb_clear_halt(h, 0x81)          ← clears halt on EP1-IN
  // NO libusb_set_interface_alt_setting()
```
- Bulk reads via **synchronous** `libusb_bulk_transfer(h, 0x81, buf, len, &transferred, timeout)`
- Single 16KB read with 2s timeout per test cycle
- `bulk_read_some()` helper at line ~1378

### rx888_stream (rx888_tools submodule)
```
open_rx888():
  find_device() + libusb_open()
  // NO libusb_detach_kernel_driver
  // NO libusb_clear_halt

pick_bulk_in_endpoint():
  scans USB descriptors, discovers iface/alt/ep

main setup:
  libusb_claim_interface(h, sep.iface)
  libusb_set_interface_alt_setting(h, sep.iface, sep.alt)  ← if alt != 0
```
- Bulk reads via **asynchronous** `libusb_submit_transfer()` with callbacks
- 32 x 1MB transfers in flight simultaneously
- Separate writer thread processes completions

### Critical Differences
| Aspect | fx3_cmd | rx888_stream |
|--------|---------|--------------|
| Alt setting | NEVER set | Conditionally set |
| Clear halt | YES (on open) | NO |
| Transfer type | Synchronous (blocking) | Asynchronous (callback) |
| Queue depth | 1 x 16KB | 32 x 1MB |
| Kernel driver detach | YES | NO |

## Hypotheses to Investigate Next

### H1: Synchronous bulk transfer is too slow to prime the DMA pipeline
- FX3 SuperSpeed bulk requires the host to have transfers queued BEFORE data arrives
- With sync transfers, there's a gap between STARTFX3 and the first bulk read submission
- During that gap, FX3 fills its 8 DMA buffers, overflows PIB, watchdog fires
- rx888_stream pre-submits 32 transfers BEFORE sending STARTFX3

### H2: xHCI host controller needs transfers queued to accept data
- On xHCI (USB 3.0), the host controller won't accept IN data unless there are
  pending transfer descriptors (TDs) in the endpoint ring
- `libusb_bulk_transfer()` submits one TD and waits — but by the time it submits,
  the FX3 has already tried to send data and gotten NAKs
- The 8 DMA buffers fill up, PIB overflows, and the endpoint wedges

### H3: Clear-halt timing issue
- `libusb_clear_halt()` is called once at device open time
- But STARTFX3 does `CyU3PUsbResetEp()` which may reset the endpoint state
- This could desynchronize the host/device endpoint state

## Suggested Fix Direction

The fix should be in `fx3_cmd.c`'s test functions, NOT in firmware. Options:
1. **Submit async transfers before STARTFX3** — mirror rx888_stream's approach
2. **Add a small async transfer helper** for tests that need bulk reads
3. **Call libusb_clear_halt() immediately before bulk_read_some()** — re-sync endpoint
4. **Increase bulk_read_some timeout and add retry** — if it's a timing race

## Test Evidence Summary
| Test | Result | Key Data |
|------|--------|----------|
| A (basic start+stats) | FAIL | dma=0, gpif=255, pll=0x01, faults=1 |
| B (debug console) | FAIL | GO s=2, DMA=8 during stall, WDG rc=68 |
| C (rx888_stream) | PASS | 60 MiB/s; after manual start: 1 bad xfer then fine |
| D (isolate gpio) | FAIL | Identical with/without gpio_extremes |
| F (settle time) | FAIL | Fails at all delays 0.1-2.0s |
| G (minimal stop/start) | FAIL | Same pattern: dma=0, gpif=255, faults accumulate |
