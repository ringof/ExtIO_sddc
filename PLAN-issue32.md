# Plan: Issue #32 — Move duplicate defines to protocol.h and fix circular include

## Problem

1. **Duplicate defines**: `FX3_CMD_BASE`, `FX3_CMD_COUNT`, and `SETARGFX3_LIST_COUNT` are defined identically in both `DebugConsole.c` (lines 61–62, 69) and `USBHandler.c` (lines 49–51).
2. **Scattered extern declarations**: `FX3CommandName[]` is defined in `DebugConsole.c` and declared `extern` in `USBHandler.c` with no shared header.
3. **Circular include**: `Application.h` includes `i2cmodule.h`, which includes `Application.h` back.

## Changes (4 files)

### 1. `protocol.h` — add the three defines and extern declarations

After the existing `_DEBUG_USB_` / `MAXLEN_D_USB` block, add:

```c
/* Debug trace: command-name lookup tables */
#define FX3_CMD_BASE           0xAA
#define FX3_CMD_COUNT          17
#define SETARGFX3_LIST_COUNT   14

#ifdef TRACESERIAL
extern const char *FX3CommandName[FX3_CMD_COUNT];
extern const char *SETARGFX3List[SETARGFX3_LIST_COUNT];
#endif
```

This gives every file that includes `protocol.h` (via `Application.h`) access to the defines and, when `TRACESERIAL` is active, to the extern declarations.

### 2. `Application.h` — move `TRACESERIAL` before `#include "protocol.h"`

Currently `TRACESERIAL` is defined on line 32, *after* `protocol.h` is included on line 30. Move it before so the `#ifdef TRACESERIAL` guard in `protocol.h` takes effect:

```c
#define TRACESERIAL     /* enable the trace to serial port */
#include "protocol.h"
```

### 3. `DebugConsole.c` — remove the three local `#define`s

Delete lines 61–62 (`FX3_CMD_BASE`, `FX3_CMD_COUNT`) and line 69 (`SETARGFX3_LIST_COUNT`). Keep the array definitions — they are the single canonical source of truth, now sized by the defines from `protocol.h`.

### 4. `USBHandler.c` — remove local defines *and* extern declarations

Delete lines 47–51 (`extern const char*` + three `#define`s). These are now supplied by `protocol.h`.

### 5. `i2cmodule.h` — remove `#include "Application.h"` (line 6)

`i2cmodule.h` only uses types from `cyu3types.h` and `cyu3usbconst.h`, both already included. The `Application.h` include is unnecessary and creates the circular dependency.

## Build verification

```
make -C SDDC_FX3 clean && make -C SDDC_FX3 all
```

## Success criteria

- Build completes with no errors or new warnings.
- No remaining duplicate definitions of the three constants (verified by grep).
- `i2cmodule.h` no longer includes `Application.h`.
