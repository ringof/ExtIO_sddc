#!/usr/bin/env bash
# create_globals_issues.sh — Create GitHub issues for globals-to-static cleanup
#
# Run once from the repo root:  bash scripts/create_globals_issues.sh
# Requires: gh CLI authenticated with repo access
#
# Idempotent: checks for existing open issues with the same title before creating.

set -euo pipefail

REPO="ringof/ExtIO_sddc"
LABEL="cleanup"

# Ensure the label exists (no-op if it already does)
gh label create "$LABEL" --description "Code cleanup and maintenance" --color "c5def5" \
  --repo "$REPO" 2>/dev/null || true

declare -a TITLES
declare -a BODIES

# --- Issue 1: DebugConsole.c globals ---
TITLES+=("Make glUARTtoCPU_Handle, glConsoleInBuffer, glConsoleInIndex static in DebugConsole.c")
BODIES+=("$(cat <<'BODY'
## Context

Code review audit of FX3 firmware globals found three variables in `SDDC_FX3/DebugConsole.c` that are only used within that translation unit but are not declared `static`:

- `glUARTtoCPU_Handle` — UART DMA channel handle, used only in `InitializeDebugConsole()` and the UART callback
- `glConsoleInBuffer[32]` — Console input accumulator, used only in the UART/USB debug input path and `ParseCommand()`
- `glConsoleInIndex` — Index into `glConsoleInBuffer`, same scope as above

## Proposed Change

Add `static` to all three declarations. Verify no other file declares them `extern` (grep confirms none do today).

## Rationale

Reducing global linkage scope prevents accidental cross-file coupling and makes the module's interface explicit. This is a mechanical change with no behavioral impact.
BODY
)")

# --- Issue 2: USBHandler.c glEp0Buffer ---
TITLES+=("Make glEp0Buffer static in USBHandler.c")
BODIES+=("$(cat <<'BODY'
## Context

`glEp0Buffer[64]` in `SDDC_FX3/USBHandler.c` is the EP0 scratch buffer used exclusively within USB vendor request handlers in that file. It is not declared `static`.

## Proposed Change

Add `static` to the `glEp0Buffer` declaration. Verify no other file references it via `extern`.

## Rationale

The buffer is an implementation detail of the EP0 handler. Making it `static` prevents accidental reuse from other modules and clarifies the module boundary.
BODY
)")

# --- Issue 3: USBHandler.c glVendorRqtCnt ---
TITLES+=("Make glVendorRqtCnt static in USBHandler.c")
BODIES+=("$(cat <<'BODY'
## Context

`glVendorRqtCnt` in `SDDC_FX3/USBHandler.c` is a vendor-request counter used only within that file for TraceSerial diagnostics. It is not declared `static`.

## Proposed Change

Add `static` to the `glVendorRqtCnt` declaration. Verify no external references exist.

## Rationale

Same as the other globals-to-static cleanups: reduce linkage scope, clarify module boundaries, prevent accidental coupling.
BODY
)")

# --- Issue 4: StartStopApplication.c glCyFxGpifName ---
TITLES+=("Make glCyFxGpifName static in StartStopApplication.c")
BODIES+=("$(cat <<'BODY'
## Context

`glCyFxGpifName` in `SDDC_FX3/StartStopApplication.c` is a string literal pointer used only in `StartGPIF()` for a debug print. It is not declared `static`.

## Proposed Change

Add `static` to the `glCyFxGpifName` declaration. Verify no external references exist. Consider making it `static const char *const` for full const-correctness.

## Rationale

The variable is a module-internal constant used for diagnostics. Making it `static` (and ideally `const`) prevents accidental external coupling and communicates intent.
BODY
)")

echo "Creating issues in $REPO..."

created=0
skipped=0

for i in "${!TITLES[@]}"; do
    title="${TITLES[$i]}"
    body="${BODIES[$i]}"

    # Check for existing open issue with same title
    existing=$(gh issue list --repo "$REPO" --state open --search "\"$title\"" --json title --jq '.[].title' 2>/dev/null || true)
    if echo "$existing" | grep -qF "$title"; then
        echo "SKIP: '$title' (already exists)"
        ((skipped++)) || true
        continue
    fi

    gh issue create --repo "$REPO" \
        --title "$title" \
        --body "$body" \
        --label "$LABEL"

    echo "CREATED: '$title'"
    ((created++)) || true
done

echo ""
echo "Done. Created: $created, Skipped: $skipped"
