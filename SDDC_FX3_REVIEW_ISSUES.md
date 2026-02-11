# SDDC_FX3 Firmware Code Review Issues

Each section below is a standalone issue ready to copy-paste into GitHub's "New Issue" form.
The `## Title:` line is the issue title, everything below it until the next `---` is the body.

---

## Title: Bug: sizeof on pointer instead of struct in StopApplication()

**File:** `SDDC_FX3/StartStopApplication.c:157`

**Severity:** High

**Description:**

```c
CyU3PMemSet((uint8_t *)&epConfig, 0, sizeof(&epConfig));
```

`sizeof(&epConfig)` evaluates to `sizeof(CyU3PEpConfig_t*)` which is 4 bytes on ARM, **not** the size of the `CyU3PEpConfig_t` struct. This means `StopApplication()` only zeroes 4 bytes of the endpoint config struct before passing it to `CyU3PSetEpConfig()` to disable the endpoint.

**Impact:** The endpoint may not be properly disabled on USB disconnect/reset events. Remaining uninitialized fields could leave stale DMA state, causing issues on reconnection.

**Fix:**
```c
CyU3PMemSet((uint8_t *)&epConfig, 0, sizeof(epConfig));
```

---

## Title: Bug: sizeof on pointer in I2C read handler zeroes only 4 bytes

**File:** `SDDC_FX3/USBhandler.c:291`

**Severity:** High

**Description:**

```c
CyU3PMemSet (glEp0Buffer, 0, sizeof (glEp0Buffer));
```

`glEp0Buffer` is declared as `uint8_t *glEp0Buffer`, so `sizeof(glEp0Buffer)` is 4 (pointer size), not the allocated buffer size of 64 bytes (`CYFX_SDRAPP_MAX_EP0LEN`).

**Impact:** In the `I2CRFX3` handler, only 4 bytes of the EP0 buffer are zeroed before the I2C read. If the I2C read returns fewer bytes than `wLength`, stale data from previous vendor requests may be sent to the host.

**Fix:**
```c
CyU3PMemSet (glEp0Buffer, 0, CYFX_SDRAPP_MAX_EP0LEN);
```

---

## Title: Bug: UINT64 typedef is actually uint32_t

**File:** `SDDC_FX3/Application.h:60`

**Severity:** Medium

**Description:**

```c
#define UINT64  uint32_t
```

This defines `UINT64` as a 32-bit type. Any code using `UINT64` expecting 64-bit storage will silently truncate values. Meanwhile, the firmware also uses real `uint64_t` in places like `USBhandler.c:358`:

```c
uint64_t freq;
freq = *(uint64_t *) &glEp0Buffer[0];
```

The coexistence of a fake `UINT64` (32-bit) and real `uint64_t` (64-bit) is confusing and error-prone.

**Fix:** Either remove the `UINT64` macro entirely and use `uint32_t` explicitly where 32-bit is intended, or fix it to `#define UINT64 uint64_t`.

---

## Title: Bug: I2C write errors silently reported as success to host

**File:** `SDDC_FX3/USBhandler.c:283-286`

**Severity:** Medium

**Description:**

```c
apiRetStatus = I2cTransfer(wIndex, wValue, wLength, glEp0Buffer, CyFalse);
if (apiRetStatus == CY_U3P_SUCCESS)
    isHandled = CyTrue;
else
{
    CyU3PDebugPrint (4, "I2cwrite Error %d\n", apiRetStatus);
    isHandled = CyTrue; // ?!?!?!
}
```

The original developer flagged this with `?!?!?!`. When an I2C write fails, `isHandled` is still set to `CyTrue`, causing the USB stack to ACK the control transfer. The host has no way to detect the failure.

**Impact:** Hardware configuration failures (tuner, clock generator, attenuator) are invisible to host software.

**Fix:** Set `isHandled = CyFalse` on I2C failure, or stall EP0 to signal the error to the host.

---

## Title: No wLength validation on EP0 vendor requests

**File:** `SDDC_FX3/USBhandler.c` (multiple locations)

**Severity:** Medium

**Description:**

The vendor request handler calls `CyU3PUsbGetEP0Data(wLength, glEp0Buffer, NULL)` throughout without validating that `wLength <= CYFX_SDRAPP_MAX_EP0LEN` (64 bytes). The `wLength` value comes directly from the USB setup packet and is controlled by the host.

If the host sends a vendor request with `wLength > 64`, this could overflow the 64-byte `glEp0Buffer`.

Affected commands: `GPIOFX3`, `STARTADC`, `I2CWFX3`, `TUNERINIT`, `TUNERSTDBY`, `TUNERTUNE`, `SETARGFX3`, `STARTFX3`, `STOPFX3`, `RESETFX3`.

**Fix:** Add a check at the top of the vendor request handler:
```c
if (wLength > CYFX_SDRAPP_MAX_EP0LEN) {
    CyU3PUsbStall(0, CyTrue, CyFalse);
    return CyTrue;
}
```

---

## Title: Race condition on debug-over-USB buffer

**File:** `SDDC_FX3/DebugConsole.c:245-250` and `SDDC_FX3/USBhandler.c:525-533`

**Severity:** Medium

**Description:**

The debug-over-USB mechanism uses a shared buffer (`bufdebug[]`) and length counter (`debtxtlen`, declared `volatile`). The writer side in `DebugPrint2USB`:

```c
if (debtxtlen+len < MAXLEN_D_USB)
{
    memcpy(&bufdebug[debtxtlen], buf, len);
    debtxtlen = debtxtlen+len;
}
```

The reader side in `READINFODEBUG` EP0 handler:

```c
uint16_t len = debtxtlen;
memcpy(glEp0Buffer, bufdebug, len);
debtxtlen=0;
```

These access the shared buffer from different execution contexts (application thread vs. USB callback) without any synchronization primitive. The `memcpy` and `debtxtlen` update are not atomic, so concurrent access can corrupt the buffer or cause partial reads.

**Fix:** Use a mutex, or disable/restore interrupts around the critical sections, or use a proper ring buffer with atomic indices.

---

## Title: Soft-float arithmetic in Si5351 PLL calculations on FPU-less ARM9

**File:** `SDDC_FX3/driver/Si5351.c:91-94, 178-181`

**Severity:** Medium

**Description:**

The Si5351 PLL setup uses `float` and `double` arithmetic:

```c
P1 = (UINT32)(128 * ((float)num / (float)denom));
// ...
f = (double)l;
f *= 1048575;
f /= xtalFreq;
```

The FX3's ARM926EJ-S core has no hardware FPU. All floating-point operations are emulated in software via compiler-generated library calls. This is slow and adds significant code size for the soft-float library.

**Impact:** Frequency changes are slower than necessary. The soft-float library consumes precious code space in the 180KB code region.

**Fix:** The Si5351 PLL register calculations (P1, P2, P3) can be performed entirely with integer arithmetic using the formulas from the Si5351 datasheet. For example:
```c
P1 = 128 * mult + ((128 * num) / denom) - 512;
P2 = 128 * num - denom * ((128 * num) / denom);
P3 = denom;
```

---

## Title: CheckStatus/CheckStatusSilent hang forever on error with no recovery

**File:** `SDDC_FX3/Support.c:139-143, 155-160`

**Severity:** Medium

**Description:**

```c
void CheckStatus(char* StringPtr, CyU3PReturnStatus_t Status)
{
    if (glDebugTxEnabled)
    {
        if (Status != CY_U3P_SUCCESS)
        {
            DebugPrint(4, "\r\n%s failed...", ...);
            while (1)
            {
                DebugPrint(4, "?");
            }
        }
    }
}
```

Any failed API call during initialization causes an infinite loop. Additionally, `IndicateError()` in `StartUP.c:17` is a no-op (`return;` immediately), so there is no LED or GPIO indication of failure either.

**Impact:** In a deployed device without UART connected, a transient initialization failure (e.g., I2C bus glitch) causes the device to hang permanently with no user-visible indication. The only recovery is physical power cycling.

**Suggestion:** Consider logging the error and calling `CyU3PDeviceReset(CyFalse)` after a timeout, or implementing `IndicateError()` to blink an LED (the commented-out PWM code is already there).

---

## Title: Hardware detection defaults to RX999 when no tuner found

**File:** `SDDC_FX3/RunApplication.c:228-231`

**Severity:** Low

**Description:**

```c
// After Si5351 init succeeds, but no I2C tuner responds:
else
{
    HWconfig = RX999;
    DebugPrint(4, "No Tuner Detectedinitialize. RX999 detected.");
}
```

If the Si5351 initializes but none of the known tuners (R820T, R828D, RD5815) respond on I2C, the firmware assumes the hardware is an RX999. This means any unknown or new hardware variant with a Si5351 will be misidentified.

**Impact:** Wrong GPIO pins could be driven, potentially causing hardware damage on unknown boards.

**Suggestion:** Default to `NORADIO` when no tuner is positively identified, and require explicit identification for RX999 (perhaps via a dedicated GPIO sense pin or I2C device).

---

## Title: PIB error callback is disabled - silent GPIF data errors

**File:** `SDDC_FX3/StartStopApplication.c:130`

**Severity:** Low

**Description:**

```c
//  CyU3PPibRegisterCallback(Pib_error_cb, CYU3P_PIB_INTR_ERROR);
```

The GPIF Parallel Interface Block error callback is commented out. This means bus errors such as GPIF data overflows, thread starvation, or protocol violations are silently ignored.

**Impact:** At high sample rates (64 MSPS+), if the USB host cannot consume data fast enough, GPIF buffers may overflow without any notification. The host would receive corrupted or discontinuous ADC samples with no indication of the problem.

**Suggestion:** Re-enable the callback and either log the error or set a flag that the host can query via the `TESTFX3` vendor command.

---

## Title: Unreachable break statement in TUNERINIT handler

**File:** `SDDC_FX3/USBhandler.c:322`

**Severity:** Low (cosmetic)

**Description:**

```c
case RX888r3:
    si5351aSetFrequencyB(freq);
    RDA5815Initial(freq);
    break;
break;  // <-- unreachable dead code
```

Two consecutive `break` statements. The second is unreachable. This is a copy-paste artifact.

---

## Title: No product string descriptors for RX888r3, RX999, RXLUCY

**File:** `SDDC_FX3/USBdescriptor.c:312-317`

**Severity:** Low

**Description:**

```c
case RX888r3:
case RX999:
case RXLUCY:
default:
    OverallStatus |= Status = CyU3PUsbSetDesc(CY_U3P_USB_SET_STRING_DESCR, 2,
        (uint8_t *)CyFxUSBProductDscr);  // Generic "SDR"
    break;
```

The BBRF103, HF103, RX888, and RX888mk2 all have dedicated product string descriptors, but the three newer variants (RX888r3, RX999, RXLUCY) fall through to the generic "SDR" string.

**Impact:** Host software cannot distinguish these hardware variants by USB product string alone. It must use the `TESTFX3` vendor command to read `HWconfig`.

---

## Title: USB 2.0 descriptor omits serial number string index

**File:** `SDDC_FX3/USBdescriptor.c:52`

**Severity:** Low

**Description:**

The USB 3.0 device descriptor specifies serial number string index 3, but the USB 2.0 descriptor sets it to 0 (none):

```c
// USB 3.0:
0x03,  /* Serial number string index */
// USB 2.0:
0x00,  /* Serial number string index */
```

**Impact:** When connected via USB 2.0, the host OS cannot distinguish between multiple identical SDDC devices by serial number. This prevents multi-device setups over USB 2.0.

---

## Title: Significant code duplication in radio attenuator/gain functions

**Files:** `SDDC_FX3/radio/rx888r2.c`, `rx888r3.c`, `rx999.c`, `hf103.c`, `rxlucy.c`

**Severity:** Low (code quality)

**Description:**

The `SetAttenuator()` and `SetGain()` functions are nearly identical across 5 radio files. They all implement the same bit-bang SPI protocol for PE4304 (6-bit attenuator) and AD8370 (8-bit VGA), differing only in GPIO pin assignments.

For example, `rx888r2_SetAttenuator`, `rx888r3_SetAttenuator`, and `rx999_SetAttenuator` are character-for-character identical. `hf103_SetAttenuator` and `rxlucy_SetAttenuator` differ only in the bit extraction (`>> 5` vs `!= 0`).

**Impact:** Wastes code space in the memory-constrained FX3 (180KB code region). Makes maintenance harder - a bug fix must be applied to 5 copies.

**Suggestion:** Extract a shared `bitbang_spi_write(gpio_le, gpio_clk, gpio_data, value, bits)` function and call it from each radio's wrapper.

---

## Title: Thread stack size (1KB) may be insufficient

**File:** `SDDC_FX3/Application.h:35`

**Severity:** Low (risk)

**Description:**

```c
#define FIFO_THREAD_STACK (0x400)  // 1024 bytes
```

The single application thread has a 1KB stack. This thread executes:
- I2C transactions (local `CyU3PI2cPreamble_t` structs)
- Si5351/R820T/ADF4351 initialization (local arrays up to 8 bytes, plus nested calls)
- `DebugPrint2USB` which allocates a 100-byte local `buf[MAXLEN_D_USB]`
- `ParseCommand` with `tx_thread_info_get` calls
- `va_list` processing in `MyDebugSNPrint`

The existing "stack" debug command (`DebugConsole.c:92-103`) exists specifically to monitor free stack space, suggesting this was a known concern during development.

**Impact:** Stack overflow would silently corrupt the heap, causing unpredictable behavior or crashes.

**Suggestion:** Consider increasing to `0x800` (2KB) and monitoring with the stack debug command during development.

---

## Title: Console input buffer off-by-one in bounds check

**File:** `SDDC_FX3/DebugConsole.c:274-276` and `SDDC_FX3/USBhandler.c:520-522`

**Severity:** Low

**Description:**

```c
ConsoleInBuffer[ConsoleInIndex] = InputChar | 0x20;
if (ConsoleInIndex++ < sizeof(ConsoleInBuffer)) ConsoleInBuffer[ConsoleInIndex] = 0;
else ConsoleInIndex--;
```

`ConsoleInBuffer` is 32 bytes. The post-increment check means:
- When `ConsoleInIndex` is 31: writes char at [31], increments to 32, `32 < 32` is false, decrements back to 31
- The null terminator for the last position is never written
- The same logic is duplicated in both `UartCallback` and the `READINFODEBUG` USB handler

**Impact:** Minor - the console command parser could read past the intended string end in edge cases. Only affects debug functionality.

---

## Title: Typos in hardware detection debug strings

**File:** `SDDC_FX3/RunApplication.c:196-231`

**Severity:** Cosmetic

**Description:**

Multiple debug strings have missing spaces:

```
"R820T Detectedinitialize. BBRF103 detected."
"R820T Detectedinitialize. RX888 detected."
"R828D Detectedinitialize. RX888r2 detected."
"RD5812 Detectedinitialize. RX888r3 detected."
"No Tuner Detectedinitialize. RX999 detected."
```

Should be e.g. `"R820T Detected. BBRF103 detected."` or similar.

Also in `rd5815.c:71`: `"refclk is out of rage"` should be `"refclk is out of range"`.
