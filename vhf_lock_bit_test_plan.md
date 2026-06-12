# Test plan — resolve the R828D PLL-lock bit (reg 0x02) before patching vhf_tune.py

## Why this exists

`vhf_tune.py` reports PLL lock from `data[2] & 0x40` (reg 0x02, mask 0x40).
We have **two** facts that, taken together, predict that mask is wrong:

1. **Empirical (already seen on the bench):** reg 0x02 mask 0x40 reads **1 on a
   fresh / untuned chip**, so `lock=YES` is unreliable.
2. **Documentary (Rafael R820T2 Register Description):** the chip streams *reads*
   LSB-first but takes *writes* MSB-first. A standard MSB-first I2C master (the
   FX3) therefore receives every read byte **bit-reversed** from the datasheet's
   logical numbering — which is exactly why reg 0x00 reads `0x69` for us while
   the datasheet lists it as `0x96`. We removed the host read-reverse (that fixed
   the ID check), but that leaves **all other status reads in wire order**.

In wire order:

| | datasheet logical | wire (what we read) | mask |
|---|---|---|---|
| script's current lock check | `VCO_INDICATOR[1]` (logical b1) | wire b6 | `0x40` |
| datasheet lock-ish MSB | `VCO_INDICATOR[6]` (logical b6) | wire b1 | `0x02` |

**Hypothesis (falsifiable):** the real lock indicator is reg 0x02 **wire bit 1
(mask 0x02)**, not wire bit 6 (mask 0x40). If so, on a known-good tune the `0x02`
bit transitions 0→1 while the `0x40` bit does not track lock.

**Do not patch the mask until this test runs.** Per CLAUDE.md, test the
falsifier first; a wrong "fix" that happens to pass at one frequency is worse
than the honest current behavior.

## What the test uses

`r828d_probe.py` snapshot/diff mode — **read-only**, safe to run while a stream
is up and against a live VHF tune:

- `--snapshot PATH [--label TEXT]` — save regs 0x00..0x1f to a file.
- `--diff BEFORE AFTER` — show which bits moved, annotated, plus a dedicated
  reg-0x02 lock analysis. Needs no device (pure file op).

## Procedure

Run on the bench RX888 mk2 with firmware loaded and the ADC clock started
(`fx3_cmd load …` / `fx3_cmd adc 64000000`, or `vhf_tune.py --load … --adc …`).

1. **Untuned baseline.** Make sure no VHF tune is active (power-cycle or just
   don't tune). Snapshot:
   ```
   python3 r828d_probe.py --snapshot /tmp/untuned.txt --label untuned
   ```

2. **Tune to a known-good frequency** that locked before (144 MHz on this
   bench), and leave it tuned (`--persist` so standby doesn't undo it):
   ```
   python3 vhf_tune.py 144000000 --persist
   ```

3. **Locked snapshot** (tuner still tuned, stream optional):
   ```
   python3 r828d_probe.py --snapshot /tmp/locked.txt --label locked-144M
   ```

4. **Diff.**
   ```
   python3 r828d_probe.py --diff /tmp/untuned.txt /tmp/locked.txt
   ```

5. *(Optional, strengthens the result)* repeat steps 2–4 at a second frequency
   (e.g. 222 MHz) into `/tmp/locked2.txt`, and also capture an **out-of-range /
   no-lock** tune (e.g. a frequency the PLL can't reach) into `/tmp/nolock.txt`
   to confirm the bit that goes 1 on lock stays 0 when it genuinely doesn't lock.

## Reading the result — decision matrix

Look at the `PLL-lock bit hypothesis (reg 0x02)` block in the diff:

| `0x02` (wire b1) | `0x40` (wire b6) | Conclusion | Action |
|---|---|---|---|
| **0 → 1** | unchanged / noisy | **Hypothesis CONFIRMED** | proceed to the fix below |
| unchanged | **0 → 1** | current mask is right; bit-order model wrong for status reads | do **not** change the mask; investigate why reg 0 still reads 0x69 (FX3 may reverse asymmetrically) |
| 0 → 1 | 0 → 1 | both move; ambiguous | use the no-lock capture (step 5) to pick the bit that stays 0 without lock |
| neither moves | | lock isn't observable in reg 0x02 on this chip | escalate: capture full diff, reconsider whether lock lives elsewhere (e.g. reg 0x03 / VCO_INDICATOR full field) |

The no-lock capture is the tie-breaker: the **real** lock bit is the one that is
**1 only when actually locked** — high after a good tune, low after a failed one.

## Follow-up fix (gated on CONFIRMED)

Only if the diff confirms the hypothesis. The clean fix is **not** to swap one
mask — it's to make `vhf_tune.py` read in the datasheet's logical bit order, the
same way librtlsdr does, so every status read lines up with the doc:

- **Bit-reverse each byte on read** (read path only; writes stay verbatim).
- Then: ID check expects **0x96** (not 0x69); lock stays `data[2] & 0x40`
  (now correctly logical `VCO_INDICATOR[6]`); `vco_fine` stays `data[4] & 0x30`
  (now correctly logical b5:4). All three become correct from one change.
- Update `r828d_probe.py` likewise (or keep it wire-order but it already labels
  every read as wire order, so it stays self-consistent either way).

That fix ships as one reviewed change with its own change-documentation block
(description / build / validation / regression) per CLAUDE.md.

## Regression check for the change itself

After the fix, re-run this exact procedure: the diff's lock bit must still go
0→1 on a good tune and stay 0 on a no-lock tune, and a normal `vhf_tune.py
144000000` must still print `lock=YES` and produce the 4.57 MHz IF carrier in
the ADC spectrum. HF direct-sampling streaming is untouched (tuner off-path) and
needs no re-test beyond confirming a normal HF capture still works.
