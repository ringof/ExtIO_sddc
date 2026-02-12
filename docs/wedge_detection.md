# GPIF Clock Loss Detection and Wedge Recovery

## The problem

When the ADC sampling clock disappears mid-stream (Si5351 PLL unlock,
I2C programming failure, power glitch), the original firmware and host
streamer software enter a deadlocked state with no recovery path short
of physically unplugging the device.

This document explains why the wedge happens, what detection mechanisms
the FX3 SDK provides, and how to implement a stop-and-wait recovery
instead of the current fail-and-wedge behavior.

---

## What happens when the ADC clock disappears

The GPIF state machine is clocked by the **internal** FX3 system clock
(`CY_U3P_SYS_CLK / 2`, ~100 MHz), not by the external ADC clock from
the Si5351.  When CLK0 stops:

1. The ADC's 16-bit parallel data outputs **freeze** at their last
   sampled value.
2. The GPIF state machine keeps running at full internal speed, reading
   the **same frozen values** every cycle.
3. DMA buffers fill normally with repeating garbage data.
4. The USB bulk stream continues -- the host receives a firehose of
   identical sample values.

The GPIF does not stall on clock loss.  It has no way to distinguish
"real ADC samples clocked by a working Si5351" from "stale pin values
read at the internal clock rate."

### How this becomes a wedge

The deadlock occurs as a **secondary failure** when the data path backs
up:

```
Clock loss
  → ADC outputs freeze
  → GPIF reads garbage at full rate
  → Host detects bad data (or crashes, or stops reading)
  → USB bulk endpoint NAKs (host not submitting transfers)
  → DMA buffers fill, none are drained
  → GPIF threads enter BUSY/WAIT states (states 5, 7, 8, 9)
  → State machine stalls indefinitely
  → glIsApplnActive remains true
  → Firmware sits idle, no timeout, no recovery
  → Device is wedged until physical disconnect
```

The firmware has no watchdog, no throughput monitor, and the PIB error
callback is commented out (`StartStopApplication.c:130`), so the entire
failure chain is silent.

---

## Detection mechanisms available in the FX3 SDK

### 1. PIB error callback (DMA-level error detection)

The SDK provides `CyU3PPibRegisterCallback()` with the
`CYU3P_PIB_INTR_ERROR` interrupt type.  The callback receives a 16-bit
argument encoding both PIB and GPIF error codes:

```c
CyU3PPibErrorType pibErr  = CYU3P_GET_PIB_ERROR_TYPE(cbArg);
CyU3PGpifErrorType gpifErr = CYU3P_GET_GPIF_ERROR_TYPE(cbArg);
```

**Relevant PIB errors** (per-thread, shown for Thread 0; Thread 1
equivalents exist at offset +8):

| Code | Name | Meaning |
|------|------|---------|
| 0x05 | `CYU3P_PIB_ERR_THR0_WR_OVERRUN` | Write beyond available buffer -- DMA can't drain fast enough |
| 0x12 | `CYU3P_PIB_ERR_THR0_SCK_INACTIVE` | Socket went inactive during transfer |
| 0x13 | `CYU3P_PIB_ERR_THR0_ADAP_OVERRUN` | DMA adapter overrun -- data rate exceeds DMA throughput |

**Relevant GPIF errors:**

| Code | Name | Meaning |
|------|------|---------|
| 0x1000 | `CYU3P_GPIF_ERR_DATA_WRITE_ERR` | Write to DMA thread that isn't ready |
| 0x2000 | `CYU3P_GPIF_ERR_INVALID_STATE` | State machine reached an invalid state |

These catch the **secondary failure** (DMA congestion after the host
stops reading), not the clock loss itself.

**Current state:** The callback is enabled (issue #10, resolved):
```c
// StartStopApplication.c:129
CyU3PPibRegisterCallback(Pib_error_cb, CYU3P_PIB_INTR_ERROR);
```

The callback body (`StartStopApplication.c:38-44`) logs the error code
to the debug console and posts the event to the `EventAvailable` queue
for host-side visibility.

### 2. DMA buffer count monitoring

The DMA callback already increments `glDMACount` on every buffer
completion (`CY_U3P_DMA_CB_PROD_EVENT`).  The application thread loops
every 100 ms.  By comparing `glDMACount` across iterations, the
firmware can detect:

- **Count stopped advancing:** DMA is stalled (USB host stopped
  reading, or GPIF stopped producing).
- **Count advancing too slowly:** Data rate is wrong (clock frequency
  incorrect or intermittent).
- **Count advancing too fast:** Internal clock is driving garbage
  through faster than expected (possible if ADC clock is absent and
  the GPIF reads stale values at 100 MHz instead of the expected
  64 MHz).

**Expected rates** at common sample rates:

| Sample rate | Buffer fill time (16 KB / 2 B per sample) | Buffers/sec |
|------------|-------------------------------------------|-------------|
| 64 MSPS | 128 us | ~7,812 |
| 32 MSPS | 256 us | ~3,906 |
| 16 MSPS | 512 us | ~1,953 |
| 4 MSPS | 2.048 ms | ~488 |

At 100 ms polling interval, even the slowest rate produces ~48 buffers
per interval.  A zero-delta is an unambiguous stall indicator.

### 3. GPIF state machine polling

`CyU3PGpifGetSMState()` returns the current state index.  The state
machine for this application has 10 states:

| State | ID | Indicates |
|-------|----|-----------|
| IDLE | 1 | Not streaming (normal when stopped) |
| TH0_RD / TH1_RD | 2, 6 | Actively reading ADC data (healthy) |
| TH0_RD_LD / TH1_RD_LD | 4, 3 | Reading with buffer load (healthy) |
| **TH0_BUSY** | **5** | Thread 0 buffer full, waiting for DMA drain |
| **TH1_BUSY** | **7** | Thread 1 buffer full, waiting for DMA drain |
| **TH0_WAIT** | **9** | Thread 0 waiting for buffer availability |
| **TH1_WAIT** | **8** | Thread 1 waiting for buffer availability |

If the state machine is stuck in a BUSY or WAIT state across multiple
consecutive polls (e.g., 3 polls = 300 ms), the DMA is congested and
the stream is effectively stalled.

This is already accessible via the `gpif` debug console command
(`DebugConsole.c`), but is not used for automated health monitoring.

### 4. Si5351 PLL lock status (proactive clock verification)

The Si5351A has a Device Status register at **I2C address 0, register
0** with the following bits:

| Bit | Name | Meaning |
|-----|------|---------|
| 7 | SYS_INIT | Device is initializing (not ready) |
| 6 | LOL_B | PLL B has lost lock |
| 5 | LOL_A | PLL A has lost lock |

Since PLL A / CLK0 drives the ADC sampling clock, reading bit 5
directly tells you whether the clock is running.  This is a **proactive**
check that detects the root cause (clock failure) rather than the
symptom (DMA stall).

The I2C bus and Si5351 driver are already initialized; reading one
register is a single `I2cTransfer()` call:

```c
uint8_t status;
I2cTransfer(0x00,         /* register address */
            0xC0,         /* Si5351 I2C address */
            1,            /* one byte */
            &status,
            CyTrue);      /* read */
if (status & 0x20) {
    /* PLL A has lost lock -- ADC clock is bad */
}
```

### 5. GPIF event callback

`CyU3PGpifRegisterCallback()` can deliver events including:

- `CYU3P_GPIF_EVT_SM_INTERRUPT` -- if the state machine is redesigned
  to fire `INTR_CPU` on error conditions (e.g., stuck in BUSY too long)
- `CYU3P_GPIF_EVT_DATA_COUNTER` -- if a data counter is configured to
  fire at a throughput threshold

These require changes to the GPIF state machine (via GPIF II Designer)
and are more invasive than the polling approaches above.

### 6. GPIF state machine redesign (advanced)

The GPIF II Designer tool could be used to modify the state machine to:

- Add a **timeout transition** from BUSY/WAIT states back to IDLE
  using a counter limit, so the SM self-recovers instead of waiting
  forever
- Fire `INTR_CPU` (beta output bit 18) when entering a BUSY state,
  giving sub-microsecond detection latency via
  `CyU3PGpifRegisterSMIntrCallback()`

This is the most responsive approach but requires regenerating
`SDDC_GPIF.h` and careful validation of the new state machine timing.

---

## Recovery strategy

### Stop-and-wait (recommended)

Instead of staying wedged, the firmware should detect the stall and
autonomously tear down the streaming pipeline, then either restart or
wait for the host to re-initiate:

```
Detect stall (any of the mechanisms above)
  │
  ├─ 1. CyU3PGpifControlSWInput(CyFalse)    // stop GPIF state machine
  │
  ├─ 2. CyU3PGpifDisable(CyFalse)           // disable SM (keep config)
  │
  ├─ 3. CyU3PDmaMultiChannelReset(&glChHandleSlFifo)  // flush DMA
  │
  ├─ 4. CyU3PUsbFlushEp(CY_FX_EP_CONSUMER)  // flush USB endpoint
  │
  ├─ 5. Read Si5351 register 0               // check PLL lock
  │     │
  │     ├─ PLL locked → clock is OK, problem was transient
  │     │   → restart GPIF + DMA (automatic recovery)
  │     │
  │     └─ PLL unlocked → clock is bad
  │         → set status flag for host to query via TESTFX3
  │         → wait for host to re-send STARTADC + STARTFX3
  │
  └─ 6. Log event via debug channel
```

### Integration points

The recovery logic belongs in `RunApplication.c:ApplicationThread()`
inside the existing 100 ms polling loop.  The PIB error callback
(`StartStopApplication.c:Pib_error_cb`) should post an event to
`EventAvailable` for deferred handling by the application thread,
since DMA and GPIF APIs cannot be safely called from interrupt
context.

### Host notification

The host can be informed of a recovery event through:

1. **TESTFX3 response** -- the 4-byte status response could encode
   a "recovered from stall" flag in an unused bit of `FWconfig` or
   `vendorRqtCnt`
2. **Debug-over-USB** -- if `flagdebug` is enabled, the recovery
   message appears in the `READINFODEBUG` buffer
3. **Stream gap** -- the host will observe a gap in the bulk transfer
   stream, which it can interpret as a recovery event

---

## Implementation priorities

| Priority | Change | Complexity | Detection latency |
|----------|--------|-----------|-------------------|
| **1** | Enable PIB error callback, post event to app thread | Low | ~ms (interrupt-driven) |
| **2** | Add `glDMACount` delta check in main loop | Low | 100-300 ms (polling) |
| **3** | Add Si5351 PLL lock check before STARTFX3 | Low | N/A (pre-flight check) |
| **4** | Add GPIF state polling in main loop | Low | 100-300 ms (polling) |
| **5** | Periodic Si5351 PLL lock check during streaming | Medium | 100-300 ms (polling) |
| **6** | Redesign GPIF state machine with timeout transitions | High | <1 us (hardware) |

Priorities 1-4 can be implemented with minimal code changes and no
hardware or state machine modifications.  They transform the silent
wedge into a detected-and-recovered condition.
