#!/bin/bash
set -u
N="${1:-200}"
PASS=0
FAIL=0
FORCE=0
CYCLES=0
TIMEOUTS=0

for i in $(seq 1 "$N"); do
    OUT=$(timeout 30 ./fx3_cmd gpif_soft_stop 2>&1)
    RC=$?
    CYCLES=$((CYCLES + 10))
    if [ "$RC" -eq 124 ]; then
        TIMEOUTS=$((TIMEOUTS + 1))
        FAIL=$((FAIL + 1))
        printf "[%s] iter=%3d TIMEOUT (device may be wedged)\n" "$(date +%H:%M:%S)" "$i"
    elif echo "$OUT" | grep -q "^PASS"; then
        PASS=$((PASS + 1))
        printf "[%s] iter=%3d PASS\n" "$(date +%H:%M:%S)" "$i"
    else
        FAIL=$((FAIL + 1))
        F=$(echo "$OUT" | grep -oE '[0-9]+/[0-9]+ cycles used force-stop' \
                        | head -1 | cut -d/ -f1)
        [ -n "$F" ] && FORCE=$((FORCE + F))
        printf "[%s] iter=%3d FAIL force=%s\n" "$(date +%H:%M:%S)" "$i" "${F:-?}"
    fi
done

echo
echo "=== gpif_soft_stop stress (N=$N invocations, ${CYCLES} cycles) ==="
echo "  Pass:        $PASS / $N"
echo "  Fail:        $FAIL / $N (of which TIMEOUTS: $TIMEOUTS)"
echo "  Force-stops: $FORCE / $CYCLES total cycles"
