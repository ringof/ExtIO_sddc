# Plan: Issue #33 — Consolidate duplicate GPIO LED setup

## Problem

`IndicateError()` in StartUp.c and `ErrorBlinkAndReset()` in Support.c both contain identical GPIO configuration for pin 21 (blue LED). The `GPIO_LED_BLUE_PIN` define is also duplicated in both files.

## Analysis

| Function | File | Purpose | Callers |
|----------|------|---------|---------|
| `IndicateError(ErrorCode)` | StartUp.c | Configure LED GPIO, set state on/off | `RunApplication.c:229` — `IndicateError(0)` turns LED off at RTOS startup |
| `ErrorBlinkAndReset()` | Support.c (static) | Configure LED GPIO, blink 10x, reset | `CheckStatus()`, `CheckStatusSilent()` |

The shared code is the 6-line GPIO override + config block. Both call `CyU3PDeviceGpioOverride` + `CyU3PGpioSetSimpleConfig` with the same pin and nearly identical `CyU3PGpioSimpleConfig_t` (only `outValue` differs).

## Proposed changes (3 files)

### 1. `Application.h` — centralize `GPIO_LED_BLUE_PIN`

Add `#define GPIO_LED_BLUE_PIN 21` alongside the existing GPIO defines. Remove the local definitions from both `.c` files.

### 2. `StartUp.c` — no logic changes

Remove the local `GPIO_LED_BLUE_PIN` define (now comes from `Application.h`). `IndicateError()` body stays as-is — it's the canonical GPIO config function.

### 3. `Support.c` — call `IndicateError()` instead of duplicating GPIO config

Remove the local `GPIO_LED_BLUE_PIN` define. Replace the GPIO config block in `ErrorBlinkAndReset()` with a call to `IndicateError(0)` (configure pin, LED off). The blink loop and reset remain.

Before:
```c
static void ErrorBlinkAndReset(void)
{
    CyU3PGpioSimpleConfig_t gpioConfig;
    int i;
    CyU3PDeviceGpioOverride(GPIO_LED_BLUE_PIN, CyTrue);
    gpioConfig.outValue    = CyFalse;
    // ...5 more lines of config...
    CyU3PGpioSetSimpleConfig(GPIO_LED_BLUE_PIN, &gpioConfig);

    for (i = 0; ...) { blink }
    CyU3PDeviceReset(CyFalse);
}
```

After:
```c
static void ErrorBlinkAndReset(void)
{
    int i;
    IndicateError(0);  /* configure LED GPIO (off) */

    for (i = 0; ...) { blink }
    CyU3PDeviceReset(CyFalse);
}
```

## Build verification

```
make -C SDDC_FX3 clean && make -C SDDC_FX3 all
```
