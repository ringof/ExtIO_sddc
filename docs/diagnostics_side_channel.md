# Firmware-Side Diagnostics for Host Streamer Software

> **Status:** Design proposal — not yet implemented.  The counters,
> `DIAGFX3` vendor command, and `fx3_diag_snapshot()` described below do
> not exist in the current firmware.

## Overview

The `glDMACount` variable, already incremented on every DMA buffer
completion, can serve as the foundation for a diagnostic side-channel
between the FX3 firmware and the host streamer (`rx888_stream`).
Combined with the RTOS tick counter and several other counters that are
either already present or trivially added, the firmware can expose a
real-time telemetry structure that lets the host detect missed samples,
clock drift, DMA congestion, USB transport errors, and other conditions
that would corrupt the SDR data pipeline.

---

## What glDMACount actually measures

Every time the GPIF fills a DMA buffer (16,384 bytes = 8,192 16-bit
samples), the DMA engine fires a `CY_U3P_DMA_CB_PROD_EVENT` callback
and `glDMACount` increments (`StartStopApplication.c:62`).

This makes `glDMACount` a **free-running sample counter at buffer
granularity**.  Since each buffer holds a fixed number of samples, and
the fill rate is determined by the ADC clock, the count rate is directly
proportional to the ADC sampling frequency:

```
buffers_per_second = sample_rate / 8192
```

| Sample rate | Expected buffers/sec | Expected count delta per 100 ms |
|------------|---------------------|-------------------------------|
| 64 MSPS | 7,812.5 | ~781 |
| 32 MSPS | 3,906.25 | ~391 |
| 16 MSPS | 1,953.125 | ~195 |
| 4 MSPS | 488.28 | ~49 |
| 2 MSPS | 244.14 | ~24 |

The firmware already has `CyU3PGetTime()` (maps to `tx_time_get()`),
which returns RTOS ticks since boot.  The default ThreadX tick on FX3
is 1 ms.  With a count and a timestamp, you have a throughput meter.

---

## Diagnostic data the firmware can expose

### Proposed telemetry structure

```c
typedef struct fx3_diag {
    /* ---- Counters (monotonically increasing, wrap at 32 bits) ---- */
    uint32_t dma_buf_count;     /* DMA buffers produced (= glDMACount) */
    uint32_t timestamp_ms;      /* RTOS tick at time of snapshot */

    /* ---- Error counters ---- */
    uint16_t pib_overrun_count; /* PIB write overrun events (thread 0+1) */
    uint16_t pib_underrun_count;/* PIB read underrun events */
    uint16_t usb_ep_underrun;   /* USB endpoint underrun events */
    uint16_t usb_phy_errors;    /* USB 3.0 PHY error count (8b10b, CRC) */
    uint16_t usb_link_errors;   /* USB 3.0 link error count */

    /* ---- Instantaneous state ---- */
    uint8_t  gpif_state;        /* Current GPIF SM state (0-9) */
    uint8_t  si5351_status;     /* Si5351 register 0: PLL lock bits */
    uint8_t  streaming;         /* glIsApplnActive */
    uint8_t  recovery_count;    /* Number of autonomous recoveries */
} fx3_diag_t;                   /* 24 bytes total */
```

This fits comfortably in the 64-byte EP0 buffer and can be delivered
as a vendor request response.

### Where each field comes from

| Field | Source | Existing? |
|-------|--------|-----------|
| `dma_buf_count` | `glDMACount` in `StartStopApplication.c` | Yes, exists |
| `timestamp_ms` | `CyU3PGetTime()` from ThreadX | Yes, SDK API |
| `pib_overrun_count` | Increment in `PibErrorCallback` on `WR_OVERRUN` | Needs callback enabled |
| `pib_underrun_count` | Increment in `PibErrorCallback` on `RD_UNDERRUN` | Needs callback enabled |
| `usb_ep_underrun` | Increment in `USBEventCallback` on `EP_UNDERRUN` | Needs counter added |
| `usb_phy_errors` | `CyU3PUsbGetErrorCounts(&phy, &link)` | Yes, SDK API |
| `usb_link_errors` | Same API, second output | Yes, SDK API |
| `gpif_state` | `CyU3PGpifGetSMState(&state)` | Yes, SDK API |
| `si5351_status` | `I2cTransfer(0x00, 0xC0, 1, &status, CyTrue)` | Needs one I2C read |
| `streaming` | `glIsApplnActive` | Yes, exists |
| `recovery_count` | New counter, increment on autonomous recovery | Needs recovery logic |

---

## Delivery mechanism

### Option A: New vendor command (recommended)

Add a new vendor request code (e.g., `DIAGFX3 = 0xB7`) that returns
the 24-byte `fx3_diag_t` structure via EP0:

```
Host sends:  bRequest=0xB7, wValue=0, wIndex=0, wLength=24, direction=IN
FX3 returns: 24 bytes of fx3_diag_t, packed little-endian
```

The host calls this periodically (every 100-500 ms) alongside its
normal streaming.  EP0 control transfers do not interfere with the
bulk data stream on EP1.

This is the cleanest approach: no change to the bulk data format, no
impact on streaming throughput, backwards-compatible (old hosts that
don't send `DIAGFX3` are unaffected).

### Option B: Extend TESTFX3

The existing `TESTFX3` (0xAC) returns only 4 bytes.  It could be
extended: if `wLength >= 24`, return the full diagnostic structure
after the original 4 bytes; if `wLength == 4`, return the legacy
format.  This avoids allocating a new command code but couples
diagnostics to the device-info query.

### Option C: In-band metadata

Embed diagnostic frames in the bulk data stream, distinguished by a
magic header.  This is the most complex option: it requires the host
to parse every buffer for metadata, adds processing overhead at
128 MB/s, and changes the sample format.  Not recommended unless the
side-channel polling of Options A/B proves insufficient.

---

## What the host can derive from the telemetry

### 1. Missed samples (firmware side)

**Method:** Compare consecutive `dma_buf_count` snapshots with the
expected count based on elapsed time and configured sample rate.

```python
dt_ms = diag.timestamp_ms - prev.timestamp_ms
expected_bufs = (sample_rate / 8192) * (dt_ms / 1000.0)
actual_bufs = diag.dma_buf_count - prev.dma_buf_count
missed_bufs = expected_bufs - actual_bufs

if missed_bufs > 0:
    missed_samples = missed_bufs * 8192
```

A positive delta means the GPIF/DMA pipeline dropped buffers before
they reached USB.  This catches:
- DMA overruns (ADC clock too fast for DMA throughput)
- GPIF thread starvation
- Buffer congestion during USB link recovery

### 2. Missed samples (USB transport)

**Method:** The host also counts received bulk transfers.  Compare
the host-side count with `dma_buf_count`:

```python
usb_transport_loss = diag.dma_buf_count - host_received_count
```

Any delta represents data that the firmware produced and sent over
USB but the host failed to receive (USB errors, dropped transfers,
host-side buffer overflow).

### 3. ADC clock drift detection

**Method:** Compute the actual buffer rate and compare to the
programmed sample rate:

```python
measured_rate = actual_bufs * 8192 / (dt_ms / 1000.0)
drift_ppm = (measured_rate - sample_rate) / sample_rate * 1e6
```

The Si5351A synthesizes the ADC clock from a 27 MHz crystal through
a fractional-N PLL.  Real-world drift sources include:

- Crystal aging and temperature coefficient (~10-50 ppm)
- PLL lock settling after frequency change
- Integer boundary spurs at certain synthesis ratios
- Power supply noise coupling into the VCO

At buffer granularity (8192 samples), the measurement resolution is:

```
1 buffer = 8192 samples
At 64 MSPS: 1 buffer = 128 us
Over a 1-second window: 7812.5 expected buffers
Resolution: 1/7812.5 = 128 ppm per buffer
```

For sub-ppm drift measurement, the host needs to accumulate over
longer windows (10+ seconds) or use the sample data itself
(correlate against a known reference).

This level of precision is sufficient to detect:
- PLL unlock/relock events (thousands of ppm)
- Gross frequency errors (wrong divider programmed)
- Temperature-induced drift over minutes (tens of ppm)

It is **not** sufficient for precision frequency calibration (which
needs sub-ppb and should use a GPS-disciplined reference or the
sample data directly).

### 4. PLL lock status

**Method:** Check `si5351_status` bit 5 directly:

```python
pll_a_locked = not (diag.si5351_status & 0x20)
pll_b_locked = not (diag.si5351_status & 0x40)
sys_init     = bool(diag.si5351_status & 0x80)
```

This is the most direct indicator of clock health.  If PLL A is
unlocked, the ADC clock is either absent or unstable and all sample
data is garbage.  The host should stop processing and alert the user
immediately.

### 5. DMA/USB health

**Method:** Monitor the error counters:

```python
if diag.pib_overrun_count > prev.pib_overrun_count:
    # GPIF is writing faster than DMA can drain
    # ADC clock may be too fast, or USB is congested

if diag.usb_ep_underrun > prev.usb_ep_underrun:
    # USB endpoint ran out of data (transient)
    # May cause a glitch in the sample stream

if diag.usb_phy_errors > threshold:
    # USB cable/connector quality issue
    # Expect data corruption or retransmissions

if diag.usb_link_errors > threshold:
    # USB link-layer problems
    # May indicate hub issues, power management interference
```

### 6. GPIF state machine health

**Method:** Check `gpif_state` for stuck conditions:

```python
BUSY_WAIT_STATES = {5, 7, 8, 9}  # TH0_BUSY, TH1_BUSY, TH1_WAIT, TH0_WAIT

if diag.gpif_state in BUSY_WAIT_STATES:
    consecutive_busy += 1
else:
    consecutive_busy = 0

if consecutive_busy > 3:  # stuck for 300+ ms
    # GPIF is stalled, DMA is congested
    # Host should consider stopping and restarting
```

Note: because the GPIF cycles through states at ~100 MHz and the
diagnostic snapshot is taken at EP0 request time, a single snapshot
showing a BUSY state is not alarming -- the state machine may be in
BUSY for a few nanoseconds between buffers during normal operation.
Only **consecutive** BUSY readings across multiple polls indicate a
real stall.

---

## Downstream impact on SDR applications

These diagnostics directly address failure modes that corrupt the SDR
signal processing pipeline:

| Condition | Effect on SDR | Diagnostic signal |
|-----------|--------------|-------------------|
| Missed samples | Phase discontinuities, spectrum artifacts, broken demodulation | `dma_buf_count` delta vs expected |
| Clock drift | Frequency offset in demodulated signals, IQ imbalance | Measured rate vs programmed rate |
| PLL unlock | Total garbage data, noise floor appears to rise to 0 dBFS | `si5351_status` bit 5 |
| USB overrun | Dropped sample blocks, periodic clicks in audio | `pib_overrun_count` increasing |
| USB link errors | Bit errors in sample data, CRC failures | `usb_phy_errors`, `usb_link_errors` |
| GPIF stall | Stream stops, application hangs | `gpif_state` stuck in BUSY/WAIT |
| Endpoint underrun | Gap in sample stream | `usb_ep_underrun` increasing |

A host application like HDSDR or SDR# (via ExtIO) could use this
telemetry to:

1. **Display a health indicator** (green/yellow/red) in the UI
2. **Annotate the waterfall** with markers where data integrity is
   compromised
3. **Automatically pause recording** when sample integrity is lost
4. **Log diagnostic events** for post-analysis of reception quality
5. **Trigger re-initialization** of the ADC clock when PLL unlock is
   detected

---

## Firmware implementation sketch

### Snapshot function

```c
void fx3_diag_snapshot(fx3_diag_t *diag)
{
    diag->dma_buf_count   = glDMACount;
    diag->timestamp_ms    = CyU3PGetTime();

    diag->pib_overrun_count  = glPibOverrunCount;   /* new global */
    diag->pib_underrun_count = glPibUnderrunCount;  /* new global */
    diag->usb_ep_underrun    = glEpUnderrunCount;   /* new global */

    CyU3PUsbGetErrorCounts(&diag->usb_phy_errors,
                           &diag->usb_link_errors);

    CyU3PGpifGetSMState(&diag->gpif_state);

    uint8_t si_status = 0;
    I2cTransfer(0x00, 0xC0, 1, &si_status, CyTrue);
    diag->si5351_status = si_status;

    diag->streaming      = glIsApplnActive ? 1 : 0;
    diag->recovery_count = glRecoveryCount;         /* new global */
}
```

### Vendor request handler addition

```c
case DIAGFX3:  /* 0xB7 */
    if (bReqType == 0xC0) {  /* vendor, device-to-host */
        fx3_diag_t diag;
        fx3_diag_snapshot(&diag);
        CyU3PMemCopy(glEp0Buffer, (uint8_t *)&diag, sizeof(diag));
        CyU3PUsbSendEP0Data(sizeof(diag), glEp0Buffer);
        isHandled = CyTrue;
    }
    break;
```

### Host-side query (libusb)

```c
fx3_diag_t diag;
int ret = libusb_control_transfer(dev,
    LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
    0xB7,           /* DIAGFX3 */
    0, 0,           /* wValue, wIndex */
    (uint8_t *)&diag,
    sizeof(diag),
    1000);          /* timeout ms */
```

---

## Cost and constraints

**Firmware cost:**
- `fx3_diag_snapshot()` takes ~50 us (dominated by the I2C read of
  Si5351 status and the `CyU3PUsbGetErrorCounts` call)
- Called only on host request via EP0, not in the streaming hot path
- No impact on bulk transfer throughput

**Memory cost:**
- ~6 new global counters (24 bytes of BSS)
- `fx3_diag_t` structure (24 bytes on stack during EP0 handler)
- Negligible compared to 512 KB SRAM

**USB bandwidth cost:**
- One 24-byte EP0 control transfer per poll interval
- EP0 is independent of the bulk streaming on EP1
- At 10 polls/second: 240 bytes/sec = 0.0002% of USB 3.0 bandwidth

**I2C bus contention:**
- The Si5351 register read shares the I2C bus with other operations
- During streaming, no other I2C traffic is expected (attenuator and
  VGA use GPIO bit-bang, not I2C)
- If contention is a concern, the Si5351 status read can be made
  conditional (only read every Nth poll, or only when `dma_buf_count`
  delta is anomalous)

---

## Appendix A: Build system and SDK compatibility

### What you need to build

All diagnostic features described in this document are **application-level
code changes** that link against the existing, unchanged SDK libraries.
The build requires:

1. **`arm-none-eabi-gcc`** cross-compiler (ARM926EJ-S target, thumb-interwork)
2. **`make`**
3. **A host GCC** to compile the `elf2img` utility from `SDK/util/elf2img/elf2img.c`

The build flow is:

```
arm-none-eabi-gcc  →  SDDC_FX3.elf  →  elf2img  →  SDDC_FX3.img
```

### SDK libraries are pre-compiled, not rebuilt

The `.a` files in `SDK/fw_lib/1_3_4/fx3_release/` are pre-compiled
ARM static archives (`elf32-littlearm`, architecture `armv5tej`).
Cypress/Infineon does not provide source code for these libraries.
They cannot be rebuilt -- only replaced with copies from a fresh SDK
install.

The repo contains a cherry-picked subset of the full FX3 SDK 1.3.4
(build 40):

| Component | Path | Purpose |
|-----------|------|---------|
| Pre-compiled libraries | `SDK/fw_lib/1_3_4/fx3_release/*.a` | 4 linked: `libcyfxapi.a`, `libcyu3threadx.a`, `libcyu3lpp.a`, `libcyu3sport.a` |
| SDK headers | `SDK/fw_lib/1_3_4/inc/` | API declarations |
| Build config | `SDK/fw_build/fx3_fw/` | Makefiles, linker scripts (`fx3.ld`), startup assembly |
| elf2img tool | `SDK/util/elf2img/elf2img.c` | Converts ELF to FX3 bootable `.img` |

### No SDK library changes needed

Every API used by the diagnostic features already exists in the
current 1.3.4 libraries:

| API | Library | Status |
|-----|---------|--------|
| `CyU3PGetTime()` | libcyu3threadx.a | Exists |
| `CyU3PUsbGetErrorCounts()` | libcyfxapi.a | Exists |
| `CyU3PGpifGetSMState()` | libcyfxapi.a | Exists |
| `CyU3PPibRegisterCallback()` | libcyfxapi.a | Exists |
| `CyU3PGpifDisable()` | libcyfxapi.a | Exists |
| `CyU3PDmaMultiChannelReset()` | libcyfxapi.a | Exists |
| `CyU3PUsbFlushEp()` | libcyfxapi.a | Exists |
| `CyU3PUsbSendEP0Data()` | libcyfxapi.a | Exists |
| `I2cTransfer()` | Application code (i2cmodule.c) | Exists |

The DIAGFX3 vendor command, PIB error callback, DMA count monitoring,
and telemetry snapshot are all additions to application `.c` files,
linked against the unchanged libraries.

### If installing a fresh FX3 SDK

If the full Infineon/Cypress FX3 SDK is installed (e.g., for the
GPIF II Designer tool or documentation), the following applies:

1. **Version match**: The repo is pinned to `CYSDKVERSION=1_3_4` in
   the makefile.  If a newer SDK version is installed, either update
   `CYSDKVERSION` or copy the new libraries into the existing
   `1_3_4/` directory structure.

2. **ABI compatibility**: Minor SDK version bumps (1.3.x) are
   backward-compatible at the API level.  A hypothetical 1.4.x or
   2.x release could break ABI.

3. **Library link order**: The makefile specifies a strict link order
   required by the GNU linker: `libcyu3sport.a` → `libcyu3lpp.a` →
   `libcyfxapi.a` → `libcyu3threadx.a` → `libc.a` → `libgcc.a`.
   A fresh SDK install's libraries slot in identically.

4. **`cyfxtx.c` template**: The memory allocator and exception
   handlers (`SDDC_FX3/cyfxtx.c`) are an SDK-provided template
   compiled with the application.  A new SDK may ship an updated
   version -- diff before replacing.

---

## Appendix B: GPIF state machine scope

### What does NOT require GPIF state machine changes

All diagnostic and telemetry features in this document work by
**observing** the existing state machine from the outside:

- `CyU3PGpifGetSMState()` reads the current state index (0-9)
  without affecting the state machine
- `glDMACount` is incremented in the DMA produce-event callback,
  independent of the GPIF configuration
- PIB error callbacks fire on DMA-level errors detected by the
  hardware, not by the state machine
- The DIAGFX3 vendor command is an EP0 control transfer, completely
  independent of the bulk streaming GPIF path

The existing `SDDC_GPIF.h` (generated by GPIF II Designer) does not
need to be modified for any feature described in this document.

### What WOULD require GPIF state machine changes

The related document `wedge_detection.md` describes a priority 6
enhancement: adding hardware-level timeout transitions so the GPIF
state machine can self-recover from stalls.  Specifically:

- The current BUSY/WAIT states (TH0_BUSY=5, TH1_BUSY=7, TH0_WAIT=9,
  TH1_WAIT=8) wait **indefinitely** for DMA buffer availability.
  The state counter (`CY_U3P_PIB_GPIF_STATE_COUNT_CONFIG`) is
  currently disabled (`0x00000000`).
- Adding a state counter timeout that transitions back to IDLE after
  N cycles would allow the state machine to autonomously break out
  of a stall.
- Firing `INTR_CPU` (beta output bit 18) on BUSY state entry would
  give sub-microsecond stall notification to the firmware.

These changes require:

1. **GPIF II Designer** (Windows GUI tool, part of the full SDK
   installer) to modify the state machine
2. Regenerating `SDDC_GPIF.h` from the modified design
3. The original `.cyfx` project file (not present in this repo --
   only the generated output exists)

**Recommendation**: The software-based polling approach (DMA count
monitoring + GPIF state polling at 100 ms intervals + PIB error
callback) provides adequate detection latency for the diagnostic
side-channel use case.  The GPIF state machine redesign is only
warranted if sub-microsecond autonomous firmware recovery is needed
(e.g., recovering within a single DMA buffer period of ~128 us at
64 MSPS).  Start with the application-level changes; consider the
state machine redesign as a future optimization if needed.
