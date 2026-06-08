#!/usr/bin/env bash
#
# cli_smoke.sh — hardware-free argument/guard smoke test for fx3_cmd.
#
# Exercises only the exit paths that fx3_cmd resolves BEFORE touching the USB
# device, so it runs anywhere fx3_cmd links (CI included) — no RX888, no
# privileges. It pins two behaviours:
#
#   1. The --no-claim guard rejects every command that is not a read-only EP0
#      read (test/stats/stats_pll/stack_check) with exit 2, BEFORE libusb_init.
#      This is the new safety interlock that keeps --no-claim from perturbing a
#      running stream; a regression here is silent and dangerous, hence a test.
#   2. The no-argument invocation prints usage and exits 2.
#   3. The allow-listed EP0 diagnostics PASS the guard: under --no-claim they
#      exit != 2 (device-absent -> 1 in CI, 0 on the bench), proving the
#      allow-list wires those four through and nothing else. (test/stats/
#      stats_pll are passive reads; stack_check also toggles firmware debug
#      mode but stays on EP0 and is permitted by design.)
#
# Exit 0 iff every case matches. Intended for `make check-cli` and CI.

set -u

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
FX3_CMD="${FX3_CMD:-$DIR/fx3_cmd}"

if [ ! -x "$FX3_CMD" ]; then
    echo "cli_smoke: $FX3_CMD not built — run 'make -C tests fx3_cmd' first" >&2
    exit 1
fi

pass=0
fail=0

# expect_exit <expected> <description> <fx3_cmd args...>
expect_exit() {
    local expected="$1" desc="$2"; shift 2
    "$FX3_CMD" "$@" >/dev/null 2>&1
    local rc=$?
    if [ "$rc" = "$expected" ]; then
        printf 'ok   %-52s (exit %s)\n' "$desc" "$rc"
        pass=$((pass + 1))
    else
        printf 'FAIL %-52s (exit %s, want %s)\n' "$desc" "$rc" "$expected"
        fail=$((fail + 1))
    fi
}

# expect_not_exit <forbidden> <description> <fx3_cmd args...>
expect_not_exit() {
    local forbidden="$1" desc="$2"; shift 2
    "$FX3_CMD" "$@" >/dev/null 2>&1
    local rc=$?
    if [ "$rc" != "$forbidden" ]; then
        printf 'ok   %-52s (exit %s, not %s)\n' "$desc" "$rc" "$forbidden"
        pass=$((pass + 1))
    else
        printf 'FAIL %-52s (exit %s, must not be %s)\n' "$desc" "$rc" "$forbidden"
        fail=$((fail + 1))
    fi
}

echo "cli_smoke: $FX3_CMD"

# --- usage ---------------------------------------------------------------
expect_exit 2 "no arguments -> usage"                            # argc < 2

# --- --no-claim guard rejects writes / recovery / harness (exit 2) -------
# These resolve before libusb_init, so they are hermetic (no USB needed).
expect_exit 2 "--no-claim gpio (write rejected)"   --no-claim gpio 0x1
expect_exit 2 "--no-claim att (write rejected)"    --no-claim att 10
expect_exit 2 "--no-claim vga (write rejected)"    --no-claim vga 128
expect_exit 2 "--no-claim adc (write rejected)"    --no-claim adc 32000000
expect_exit 2 "--no-claim i2cw (write rejected)"   --no-claim i2cw 0x12 0x00 0xff
expect_exit 2 "--no-claim reset (recovery rejected)"   --no-claim reset
expect_exit 2 "--no-claim reload (upload rejected)"    --no-claim reload
expect_exit 2 "--no-claim start (harness rejected)"    --no-claim start

# --- --no-claim allow-list passes the guard (exit != 2) ------------------
# Past the guard these reach open_rx888(); with no device attached they exit 1
# in CI (0 on the bench). Either way, exit 2 would mean the guard wrongly
# rejected a read command, which is the regression we are pinning against.
expect_not_exit 2 "--no-claim test (allowed)"        --no-claim test
expect_not_exit 2 "--no-claim stats (allowed)"       --no-claim stats
expect_not_exit 2 "--no-claim stats_pll (allowed)"   --no-claim stats_pll
expect_not_exit 2 "--no-claim stack_check (allowed)" --no-claim stack_check

echo "cli_smoke: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
