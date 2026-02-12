# Plan: Rename Inconsistent Functions to PascalCase (Issue #31)

## Summary

Rename 6 functions from mixed naming conventions to consistent PascalCase,
updating all references in source code, documentation, and tests.

## Scope — Complete Reference Map

Each rename below lists **every** occurrence found by a full-repo grep.

### 1. `Pib_error_cb` → `PibErrorCallback`

| File | Line | Context |
|------|------|---------|
| `SDDC_FX3/StartStopApplication.c` | 38 | Function definition |
| `SDDC_FX3/StartStopApplication.c` | 124 | `CyU3PPibRegisterCallback(Pib_error_cb, ...)` |
| `tests/fx3_cmd.c` | 592 | Comment: `Pib_error_cb fires` |
| `tests/fx3_cmd.c` | 596 | Comment: `GPIF overflow → Pib_error_cb → ...` |
| `docs/diagnostics_side_channel.md` | 83 | Table: `Increment in Pib_error_cb on WR_OVERRUN` |
| `docs/diagnostics_side_channel.md` | 84 | Table: `Increment in Pib_error_cb on RD_UNDERRUN` |
| `docs/wedge_detection.md` | 94 | Code block: `CyU3PPibRegisterCallback(Pib_error_cb, ...)` |
| `docs/wedge_detection.md` | 245 | Prose: `StartStopApplication.c:Pib_error_cb` |
| `PLAN.md` | 93 | Table row (plan file, not functional) |

### 2. `setupPLL` → `SetupPLL`

| File | Line | Context |
|------|------|---------|
| `SDDC_FX3/driver/Si5351.c` | 82 | Function definition (file-scope, no `static` keyword but not exported via header) |
| `SDDC_FX3/driver/Si5351.c` | 179 | Call in `si5351aSetFrequencyA` |
| `SDDC_FX3/driver/Si5351.c` | 240 | Call in `si5351aSetFrequencyB` |
| `PLAN.md` | 94 | Table row (plan file) |

### 3. `setupMultisynth` → `SetupMultisynth`

| File | Line | Context |
|------|------|---------|
| `SDDC_FX3/driver/Si5351.c` | 111 | Function definition (file-scope, no `static` keyword but not exported via header) |
| `SDDC_FX3/driver/Si5351.c` | 185 | Call in `si5351aSetFrequencyA` |
| `SDDC_FX3/driver/Si5351.c` | 247 | Call in `si5351aSetFrequencyB` |
| `PLAN.md` | 95 | Table row (plan file) |

### 4. `Si5351init` → `Si5351Init`

| File | Line | Context |
|------|------|---------|
| `SDDC_FX3/driver/Si5351.c` | 53 | Function definition |
| `SDDC_FX3/driver/Si5351.h` | 3 | Function prototype |
| `SDDC_FX3/RunApplication.c` | 138 | Call: `Status = Si5351init();` |
| `SDDC_FX3/RunApplication.c` | 141 | DebugPrint string: `"Si5351init failed to..."` |
| `PLAN.md` | 96 | Table row (plan file) |

### 5. `USBEvent_Callback` → `USBEventCallback`

| File | Line | Context |
|------|------|---------|
| `SDDC_FX3/USBhandler.c` | 332 | Function definition |
| `SDDC_FX3/USBhandler.c` | 401 | `CyU3PUsbRegisterEventCallback(USBEvent_Callback)` |
| `docs/diagnostics_side_channel.md` | 85 | Table: `Increment in USBEvent_Callback on EP_UNDERRUN` |
| `docs/architecture.md` | 377 | Prose: `**USBEvent_Callback** -- called for USB bus events` |
| `docs/architecture.md` | 465 | Diagram: `SET_CONFIGURATION triggers USBEvent_Callback` |
| `PLAN.md` | 97 | Table row (plan file) |

**NOTE:** The issue mentions `StartUP.c` but this function does NOT appear there.
It is registered in `USBhandler.c:401` inside `InitializeUSB()`.

### 6. `LPMRequest_Callback` → `LPMRequestCallback`

| File | Line | Context |
|------|------|---------|
| `SDDC_FX3/USBhandler.c` | 374 | Function definition |
| `SDDC_FX3/USBhandler.c` | 402 | `CyU3PUsbRegisterLPMRequestCallback(LPMRequest_Callback)` |
| `docs/architecture.md` | 379 | Prose: `**LPMRequest_Callback** -- called when...` |
| `PLAN.md` | 83 | Table: `Commented-out DebugPrint in LPMRequest_Callback` |
| `PLAN.md` | 98 | Table row (plan file) |

**NOTE:** Same as above — not in `StartUP.c`.

---

## Files Modified (by file)

| File | Renames applied |
|------|----------------|
| `SDDC_FX3/StartStopApplication.c` | `Pib_error_cb` → `PibErrorCallback` (×2) |
| `SDDC_FX3/driver/Si5351.c` | `setupPLL` → `SetupPLL` (×3), `setupMultisynth` → `SetupMultisynth` (×3), `Si5351init` → `Si5351Init` (×1) |
| `SDDC_FX3/driver/Si5351.h` | `Si5351init` → `Si5351Init` (×1) |
| `SDDC_FX3/RunApplication.c` | `Si5351init` → `Si5351Init` (×2, including debug string) |
| `SDDC_FX3/USBhandler.c` | `USBEvent_Callback` → `USBEventCallback` (×2), `LPMRequest_Callback` → `LPMRequestCallback` (×2) |
| `tests/fx3_cmd.c` | `Pib_error_cb` → `PibErrorCallback` (×2, comments only) |
| `docs/diagnostics_side_channel.md` | `Pib_error_cb` → `PibErrorCallback` (×2), `USBEvent_Callback` → `USBEventCallback` (×1) |
| `docs/wedge_detection.md` | `Pib_error_cb` → `PibErrorCallback` (×2) |
| `docs/architecture.md` | `USBEvent_Callback` → `USBEventCallback` (×2), `LPMRequest_Callback` → `LPMRequestCallback` (×1) |

**PLAN.md** references are **not** updated (they describe the issue, not functional code).

---

## What is NOT renamed (intentionally)

- `rx888r2_*` functions — intentional radio-driver namespacing (per issue).
- `si5351aSetFrequencyA`, `si5351aSetFrequencyB` — not listed in the issue.
- `DmaCallback`, `CyFxSlFifoApplnUSBSetupCB` — not listed in the issue.
- Debug strings that reproduce function names as user-visible output
  (e.g., `DebugPrint(4, "Si5351init failed...")`): these WILL be updated
  to match the new name so debug output is accurate.

---

## Validation Strategy

### Pre-rename baseline capture

Before any edits, capture the symbol tables of the current build output
to create a baseline for comparison:

```
arm-none-eabi-nm SDDC_FX3/SDDC_FX3.elf | sort > /tmp/symbols_before.txt
```

### Post-rename validation (5 checks)

#### Check 1: Build verification
```
cd SDDC_FX3 && make clean && make all
```
Must complete with zero errors. Any undefined-reference or implicit-function-declaration
warning means a reference was missed.

#### Check 2: Symbol table comparison
```
arm-none-eabi-nm SDDC_FX3/SDDC_FX3.elf | sort > /tmp/symbols_after.txt
diff /tmp/symbols_before.txt /tmp/symbols_after.txt
```
Expected diff: exactly 6 symbol names change (old names disappear, new names appear
at the same addresses). No other symbols should change. The text/data/bss section
sizes should be identical.

#### Check 3: Exhaustive grep for old names
```
grep -rn "Pib_error_cb\|setupPLL\|setupMultisynth\|Si5351init\|USBEvent_Callback\|LPMRequest_Callback" \
  --include='*.c' --include='*.h' --include='*.md' .
```
Expected: zero hits in source/header files; only hits in PLAN.md (which is not updated).

#### Check 4: Binary size comparison
```
arm-none-eabi-size SDDC_FX3/SDDC_FX3.elf
```
Before and after must have identical text, data, and bss sizes. Pure renames
do not change code generation — any size difference indicates an unintended
behavioral change.

#### Check 5: Host test code compilation
```
cd tests && make clean && make
```
Must compile cleanly (the only changes to fx3_cmd.c are in comments, so this
verifies no accidental code breakage).

---

## Risk Assessment

- **Low risk**: `setupPLL`, `setupMultisynth` are file-local (only used within
  Si5351.c). `Si5351init` crosses one file boundary (Si5351.h → RunApplication.c).
  All three are simple mechanical renames.

- **Low risk**: `Pib_error_cb` is used only in StartStopApplication.c (definition +
  one registration call). No header declaration.

- **Low risk**: `USBEvent_Callback` and `LPMRequest_Callback` are used only in
  USBhandler.c (definition + one registration call each). No header declarations.

- **Zero functional risk from docs/test changes**: All doc and test changes are
  in comments or markdown prose — they cannot affect compilation or runtime behavior.
