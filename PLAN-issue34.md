# Plan: Issue #34 — Rename files to consistent PascalCase

## Goal

Rename three source files whose casing is inconsistent with the rest of
the codebase:

| Current name | New name |
|---|---|
| `SDDC_FX3/USBdescriptor.c` | `SDDC_FX3/USBDescriptor.c` |
| `SDDC_FX3/USBhandler.c` | `SDDC_FX3/USBHandler.c` |
| `SDDC_FX3/StartUP.c` | `SDDC_FX3/StartUp.c` |

No corresponding `.h` headers exist for these files; no `#include`
directives reference them.

---

## Impact analysis

| Area | Impact |
|---|---|
| **Build system** | `SDDC_FX3/makefile` lines 41, 43, 44 — update `USBSOURCE` filenames |
| **Eclipse project** | `.project` / `.cproject` — no filename references found |
| **`#include` directives** | None — these are `.c` compilation units, not headers |
| **Documentation** | 6 files contain old filenames (see Step 3 below) |

---

## Steps

### Step 1: Rename the three source files via `git mv`

```
git mv SDDC_FX3/USBdescriptor.c SDDC_FX3/USBDescriptor.c
git mv SDDC_FX3/USBhandler.c    SDDC_FX3/USBHandler.c
git mv SDDC_FX3/StartUP.c       SDDC_FX3/StartUp.c
```

### Step 2: Update `SDDC_FX3/makefile`

Change the `USBSOURCE` variable (lines 41, 43, 44):

| Old | New |
|---|---|
| `StartUP.c` | `StartUp.c` |
| `USBdescriptor.c` | `USBDescriptor.c` |
| `USBhandler.c` | `USBHandler.c` |

### Step 3: Update documentation references

Files that mention the old filenames:

| File | Approximate references |
|---|---|
| `docs/architecture.md` | 6 (table rows, section heading, source reference table) |
| `docs/debugging.md` | 7 (`USBhandler.c:NNN` line references) |
| `SDDC_FX3/docs/debugging.md` | 2 |
| `PLAN.md` | ~15 |
| `PLAN-issue31.md` | ~10 |
| `PLAN-issue32.md` | ~4 |
| `PLAN-issue33.md` | ~4 |

Each occurrence will be updated to the new PascalCase name.

### Step 4: Verify build

```
cd SDDC_FX3 && make clean && make all
```

Must complete with zero errors and produce `SDDC_FX3.img`.

---

## Risk assessment

**Low risk.** Git tracks renames cleanly; there are no header files or
`#include` directives involved. The only code-affecting change is three
lines in the makefile. All other changes are documentation-only.

## Verification

- `make clean && make all` succeeds
- `git diff --stat` shows renames (100% similarity) plus makefile + docs edits
