#!/usr/bin/env bash
# tests/bench/bench.sh — unified bench test runner.
#
# Dispatches individual tests, handling host vs container execution
# transparently.  Tests that need ka9q-radio tools (powers, pcmrecord,
# decode_ft8, wsprd) run inside the container via docker exec; tests
# that talk directly to host USB devices run on the host.
#
# Usage:
#   bench.sh <test> [args...]    run a single test
#   bench.sh all                 run all in dependency order
#   bench.sh list                show available tests
#
# Environment:
#   CONTAINER   docker container name (default: ka9q-radio)
#   SKIP_AUDIO  set to 1 to skip the audio test (bench has no open audio port)

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONTAINER="${CONTAINER:-ka9q-radio}"

# ---------------------------------------------------------------------------
# Dispatch table
# ---------------------------------------------------------------------------
# Format: test_name:location:command
#   location = host | container
#   command  = script path relative to $HERE (host) or /usr/local/lib/bench/ (container)

TESTS=(
    "g0-ft8:host:run_g0_ft8.sh"
    "g0-wspr:host:run_g0_wspr.sh"
    "cat:host:cat_test.py"
    "audio:host:audio_test.py"
    "loopback:container:loopback_test.py"
    "ft8:container:ft8_test.py"
    "wspr:container:wspr_test.py"
)

# Dependency order for 'all' — tests run in this sequence.
ALL_ORDER=(g0-ft8 g0-wspr cat audio loopback ft8 wspr)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

die() { echo "bench: ERROR — $*" >&2; exit 1; }

lookup() {
    local name="$1"
    for entry in "${TESTS[@]}"; do
        local t_name="${entry%%:*}"
        if [[ "$t_name" == "$name" ]]; then
            echo "$entry"
            return 0
        fi
    done
    return 1
}

check_container() {
    if ! docker exec "$CONTAINER" true 2>/dev/null; then
        die "container '$CONTAINER' is not running (set CONTAINER env var to override)"
    fi
}

run_test() {
    local name="$1"; shift
    local entry
    entry="$(lookup "$name")" || die "unknown test: $name (try: bench.sh list)"

    local location command
    IFS=: read -r _ location command <<< "$entry"

    case "$location" in
        host)
            if [[ "$command" == *.sh ]]; then
                exec bash "$HERE/$command" "$@"
            else
                exec python3 -u "$HERE/$command" "$@"
            fi
            ;;
        container)
            check_container
            local tty_flag=""
            if [ -t 0 ]; then
                tty_flag="-it"
            fi
            # shellcheck disable=SC2086
            exec docker exec $tty_flag "$CONTAINER" \
                python3 -u "/usr/local/lib/bench/$command" "$@"
            ;;
        *)
            die "bad location '$location' for test '$name'"
            ;;
    esac
}

cmd_list() {
    echo "Available bench tests:"
    echo ""
    printf "  %-12s %-10s %s\n" "NAME" "WHERE" "SCRIPT"
    printf "  %-12s %-10s %s\n" "----" "-----" "------"
    for entry in "${TESTS[@]}"; do
        local name location command
        IFS=: read -r name location command <<< "$entry"
        printf "  %-12s %-10s %s\n" "$name" "$location" "$command"
    done
    echo ""
    echo "Run:  bench.sh <name> [args...]"
    echo "      bench.sh all              (run all in order)"
    echo ""
    echo "Container tests use CONTAINER=${CONTAINER}"
}

cmd_all() {
    local passed=() failed=()

    for name in "${ALL_ORDER[@]}"; do
        local entry
        entry="$(lookup "$name")" || die "bad ALL_ORDER entry: $name"

        local location command
        IFS=: read -r _ location command <<< "$entry"

        # Skip audio test when SKIP_AUDIO=1 (bench has no open audio port)
        if [[ "$name" == "audio" && "${SKIP_AUDIO:-0}" == "1" ]]; then
            echo ""
            echo "=== bench: $name ==="
            echo "bench: SKIP $name (SKIP_AUDIO=1)"
            continue
        fi

        echo ""
        echo "=== bench: $name ==="

        local rc=0
        case "$location" in
            host)
                if [[ "$command" == *.sh ]]; then
                    bash "$HERE/$command" || rc=$?
                else
                    python3 -u "$HERE/$command" || rc=$?
                fi
                ;;
            container)
                if ! docker exec "$CONTAINER" true 2>/dev/null; then
                    echo "bench: SKIP $name — container '$CONTAINER' not running"
                    failed+=("$name(skip)")
                    continue
                fi
                docker exec "$CONTAINER" \
                    python3 -u "/usr/local/lib/bench/$command" || rc=$?
                ;;
        esac

        if [[ $rc -eq 0 ]]; then
            passed+=("$name")
        else
            failed+=("$name")
        fi
    done

    # Summary
    echo ""
    echo "==============================="
    echo "  BENCH SUMMARY"
    echo "==============================="
    if [[ ${#passed[@]} -gt 0 ]]; then
        echo "  PASS: ${passed[*]}"
    fi
    if [[ ${#failed[@]} -gt 0 ]]; then
        echo "  FAIL: ${failed[*]}"
    fi
    echo "  ${#passed[@]}/${#ALL_ORDER[@]} passed"
    echo "==============================="

    if [[ ${#failed[@]} -gt 0 ]]; then
        exit 1
    fi
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

if [[ $# -eq 0 ]]; then
    cmd_list
    exit 0
fi

case "$1" in
    list|--list|-l)
        cmd_list
        ;;
    all)
        cmd_all
        ;;
    -h|--help|help)
        echo "Usage: bench.sh <test> [args...] | bench.sh all | bench.sh list"
        echo ""
        cmd_list
        ;;
    *)
        name="$1"; shift
        run_test "$name" "$@"
        ;;
esac
