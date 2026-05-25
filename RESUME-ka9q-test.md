# Resume — ka9q_test.sh bench validation

Branch `claude/shdn-when-stopped` (PR #132). Where we left off mid-bench-run.

## Green / done

- **Firmware SHDN (#131)** — implemented + verified in code (rx888r2.c,
  USBHandler.c, radio.h, CHANGELOG, docs). Core objective met.
- **ka9q_smoke.sh** — PASSED on the bench: full 0..fs/2 sweep, mean
  -132.19 dB, spread 58.02 dB (fs/2 alias spike), PASS. The "firmware
  streams REAL samples, no receiver UI" proof works.
- **powers patches** (01 freq-double, 02 rbw-float) and **hf_sweep.sh**
  tile fixes (warm-up + fixed MAXBINS) — verified flat edge-to-edge.
- Container reverted to ka9q-radio 42273761 + hack_no_usb_reset (the
  6a5094ac bump segfaults — audit §10, report upstream later).
- Docs (tests/README, docker README, PLAN-SHDN integration gate) updated.

## Open — ka9q_test.sh (the soak harness) is NOT green yet

Last run (`--duration 150 --reload-interval 60 --stream-secs 12`) showed
TWO distinct problems. The readiness-wait fix (commit c4e2f1d) works — it no
longer races — but it exposed:

1. **Cycle 1: radiod fully up (`rx888 running`, `hf.local` registered) but
   `powers` capture returns nothing** in the `ka9q-radio-soak` container.
   The SAME powers call worked against the `ka9q.sh`-started container during
   smoke. So the soak container's data plane differs somehow. NOT explained.
2. **Cycles 2/3: radiod stalls in rx888 init on restart** — reaches "found
   rx888 / Si5351 programmed" but never `rx888 running` within 30s. Plain
   restarts (no reload). Restart-path problem. NOT explained.

## Next step (do this FIRST, before any code change)

A `ka9q-dbg` container should still be running on the bench. Run the exact
capture command with **stderr visible** (the script hides it with
`2>/dev/null`) to find out WHY cycle 1's powers returns empty:

```bash
docker exec ka9q-dbg sh -c 'timeout 10 powers -c 1 -i 2 -f 10000000 -b 256 -w 10000 -s 30303 hf.local'; echo "exit=$?"
```

(If `ka9q-dbg` is gone, recreate it — see the chat for the full `docker run`
+ radiod-start sequence.) That output decides whether #1 is a harness bug or
something real. Then probe #2 by stopping/restarting radiod in `ka9q-dbg`
and watching whether the 2nd start ever reaches `rx888 running`.

Reminder: do not theorize the fix from the symptom — get the stderr first.
