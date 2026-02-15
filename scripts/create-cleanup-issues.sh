#!/bin/bash
# Create GitHub issues for code-review follow-up items.
# Run once from a machine with `gh` CLI authenticated.
# Usage: bash scripts/create-cleanup-issues.sh

set -euo pipefail

REPO="ringof/ExtIO_sddc"
LABEL_CLEANUP="cleanup"
LABEL_DOCS="documentation"

# Ensure labels exist (ignore error if they already do)
gh label create "$LABEL_CLEANUP" --repo "$REPO" \
   --description "Code quality / cleanup tasks" \
   --color "c5def5" 2>/dev/null || true
gh label create "$LABEL_DOCS" --repo "$REPO" \
   --description "Documentation updates" \
   --color "0075ca" 2>/dev/null || true

create_issue() {
    local title="$1"
    local body="$2"
    local label="$3"
    if gh issue list --repo "$REPO" --search "\"$title\" in:title" --json number -q '.[0].number' | grep -q .; then
        echo "SKIP (exists): $title"
    else
        gh issue create --repo "$REPO" --title "$title" --body "$body" --label "$label"
        echo "CREATED: $title"
    fi
}

# ── Issue 1: Replace uint32_t* casts with memcpy in EP0 trace printing ──

create_issue \
  "Replace uint32_t* casts with memcpy in EP0 trace/payload code" \
  "$(cat <<'EOF'
## Problem

In `USBHandler.c`, EP0 payload values are read via `*(uint32_t *)` pointer casts:

- `TraceSerial()` lines 76, 81 — `*(uint32_t *)pdata` for GPIO and ADC frequency trace output
- GPIOFX3 handler line 182 — `*(uint32_t *)&glEp0Buffer[0]` for GPIO bitmask
- STARTADC handler line 192 — `*(uint32_t *)&glEp0Buffer[0]` for frequency

These work correctly on FX3 because `glEp0Buffer` is SDK-allocated and 4-byte aligned, but they are technically undefined behavior in C and would break on platforms with strict alignment requirements.

## Proposed Fix

Replace each `*(uint32_t *)ptr` with a `memcpy` into a local `uint32_t`:

```c
uint32_t val;
memcpy(&val, &glEp0Buffer[0], sizeof(val));
```

This is a defensive/style improvement — no functional change expected.

**Priority:** Low

## Files
- `SDDC_FX3/USBHandler.c` — lines 76, 81, 182, 192
EOF
)" "$LABEL_CLEANUP"

# ── Issue 2: Add ISR/thread context comments to all callbacks ──

create_issue \
  "Add ISR/thread context comments to all callback functions" \
  "$(cat <<'EOF'
## Problem

FX3 firmware has callbacks invoked in different execution contexts (ISR vs thread). Blocking in ISR context is a common source of hard-to-debug FX3 bugs. Some callbacks already have context documentation, but coverage is incomplete:

| Callback | File | Context | Documented? |
|----------|------|---------|-------------|
| `DmaCallback` | StartStopApplication.c:50 | ISR (DMA producer event) | Partial (has event comment, no context tag) |
| `PibErrorCallback` | StartStopApplication.c:41 | ISR (PIB interrupt) | **No** |
| `USBEventCallback` | USBHandler.c:394 | Thread (USB event handler) | **No** |
| `ConsoleAccumulateChar` | DebugConsole.c:252 | Thread (USB EP0 context) | Yes |

## Proposed Fix

Add a one-line context comment at the top of each callback body:

```c
/* ISR context — must not block, no CyU3PThread* calls. */
```
or
```c
/* Thread context — safe to call blocking APIs. */
```

This prevents future developers from accidentally adding blocking calls (DebugPrint, I2C transfers, mutex waits) in ISR callbacks.

**Priority:** Low (documentation/defensiveness)

## Files
- `SDDC_FX3/StartStopApplication.c` — `PibErrorCallback`, `DmaCallback`
- `SDDC_FX3/USBHandler.c` — `USBEventCallback`
EOF
)" "$LABEL_CLEANUP"

# ── Issue 3: Clean up heritage naming drift (AD8340, stale command tables) ──

create_issue \
  "Clean up heritage naming drift from ExtIO_sddc / RX888 mk1" \
  "$(cat <<'EOF'
## Problem

The firmware carries naming artifacts from the original ExtIO_sddc host-side driver and RX888 mk1 hardware that no longer apply to the mk2 board. These create confusion for new contributors:

### AD8340 vs AD8370
The RX888 mk2 uses an **AD8370** VGA. Any remaining references to "AD8340" in comments, variable names, or documentation should be updated.

### Stale command names / placeholders
Vendor command tables (in docs or code comments) may still list removed tuner commands (`TUNERINIT`, `TUNERTUNE`, `TUNERSTDBY`) without clearly marking them as removed/legacy. `fx3_cmd.c` retains these defines for stale-command regression tests, which is intentional — but the purpose should be documented (see issue #68 if it exists, otherwise add a comment).

### Proposal-style language in docs
Some documentation (e.g., `docs/wedge_detection.md`) was written as design proposals before implementation. Language like "the firmware has no watchdog" is now factually wrong and should be updated to reflect current state.

## Scope

This is a sweep-and-fix issue. The goal is consistency, not restructuring:
1. Grep for `AD8340` — update to `AD8370` where it refers to the mk2 VGA
2. Grep for `TUNERINIT` / `TUNERTUNE` / `TUNERSTDBY` — ensure each site has a "removed/legacy" comment
3. Review `docs/wedge_detection.md` — update proposal language to match implementation
4. Scan for other mk1-era references that no longer apply

**Priority:** Low-medium (readability/onboarding)

## Files (likely affected)
- `SDDC_FX3/` source and headers — AD8340 references
- `docs/wedge_detection.md` — proposal language
- `docs/architecture.md` — command table heritage entries
- `tests/fx3_cmd.c` — stale command defines (comment only)
EOF
)" "$LABEL_CLEANUP"

echo ""
echo "Done. Created 3 issues."
