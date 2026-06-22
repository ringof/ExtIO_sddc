# PPS in-band marker — firmware investigation request

(From rx888-tools side, based on host-side measurements)

## What we've established host-side (measured, not inferred)

Rig: RX888 mk II, 129.6 MSPS, `PPS_CTL_ENABLE=1`, 3 h runs, sleep-inhibited.

- **The marker causes the loss; the bare stream does not.** `stream_soak` (identical instrumentation, no marker) is **lossless** at 129.6 MSPS. With the 1 Hz marker: **~41 ppm** loss. Same data rate both ways, so it's not throughput/backpressure.
- **The loss is inside the FX3, on the DMA→USB drain.** `glDMACount` increments on `CY_U3P_DMA_CB_PROD_EVENT` (producer commits a buffer *into* the channel, pre-USB). We see `produced > delivered` ⇒ buffers entered the channel but **never drained out the consumer socket**. Host-side / USB-wire loss is ruled out (lossless control + the loss tracks a device-internal quantity the host can't see — see next point).
- **The loss is rare, large, quantized, and boundary-locked:**
  - ~**3.7%** of markers (≈400 of 10,800) produce a loss event.
  - Each loses **~98,300 samples ≈ exactly 12 DMA buffers** (median; 98304 = 12 × 8192 samples = 12 × 16 KB). The size is **stable across runs**.
  - Loss is **~26× enriched** when the marker fires near a buffer boundary — i.e. when the forced partial transfer is **near-empty** (just after a boundary) or **near-full** (just before one), measured as `minxfer / 524288`.
- **Device is pristine throughout:** `PIB = 0`, `bad_xfers = 0`, `faults = 0`, boot count unchanged. The drop is **silent** — nothing we can read via GETSTATS moves.
- **Edge quality is not the cause.** Swapping the 100 kΩ CTL[2] series resistor for **1 kΩ** (~100× faster edge): spurious shorts **403 → 0** and displaced markers **61 → 32** (fidelity improved — the slow edge had been chattering the TOGGLE comparator), but **loss unchanged (42.3 → 41.2 ppm) and boundary-enrichment intact (26.9× → 25.8×), identical dip size.** So this is **not** metastability on the edge; it's a structural collision.

## The hypothesis we need firmware to confirm or refute

> When the GPIF forces the PPS partial commit (`TH0_PPS_COMMIT` / `TH1_PPS_COMMIT`) and that commit lands **near a DMA buffer boundary** — coinciding with the ping-pong handoff / a natural buffer completion — it desyncs the channel/socket state and **orphans ~12 already-counted buffers** in the DMA→USB drain. Those buffers were committed into the channel (so `glDMACount` counted them) but are never consumed to USB.

## Questions (ranked by how much they'd narrow it)

1. **What does `PPS_COMMIT` actually do at the socket level?** Is the forced commit a wrap-up of the **consumer** (UIB) socket, the **producer** (PIB) socket, or a GPIF-thread switch? Is it equivalent to a `CyU3PDmaChannelSetWrapUp` on a specific socket, and which one? (We need to know which side the "commit" acts on, because the loss is on the *consumer* drain.)

2. **Confirm the buffer topology.** We're assuming: 16 KB DMA buffers; 64 buffers per 524288-sample (1 MB) USB transfer; two producer sockets `PIB_SOCKET_0/1` ping-ponging; one consumer socket. What's the actual buffer **count** allocated, per socket, and the channel depth? (This bounds the in-flight set the orphan comes from.)

3. **What happens when the forced commit coincides with a natural buffer completion?** Specifically at near-empty (a buffer just completed, the next barely started) and near-full (a buffer about to complete). Can a forced wrap-up racing the DMA hardware's own commit of the same/adjacent buffer:
   - double-commit or skip a descriptor,
   - leave the consumer socket's descriptor pointer desynced,
   - strand the buffers queued behind the collision point?

4. **The ~12-buffer quantum — what sets it?** 12 × 16 KB is suspiciously clean. Does 12 correspond to anything concrete: a socket's buffer allotment, the in-flight/queued depth at the drain, a descriptor group size, 2× a ping-pong group? Whatever determines the *size* of the orphan likely names the mechanism.

5. **Can the commit be made boundary-aware?** Can the GPIF state machine see its position within the current buffer (a `BYTE_COUNT` / address counter) and **defer the `PPS_COMMIT` out of the danger band** to the next safe phase — recording the deferred sample offset so the marker's timing is still recoverable? What does the GPIF II Designer flow allow here (extra states, counter compares)?

6. **Is the failure truly silent, or is there an unexposed status?** Even for a hardware commit, is there a PIB/UIB socket error, descriptor-wrap flag, or DMA-channel error state that fires on this collision but isn't surfaced in GETSTATS? If there's *any* counter that ticks, exposing it would let us correlate host-side loss with the firmware event directly (and validate or kill this hypothesis immediately).

## Experiments / instrumentation that would discriminate

- **A drop/orphan counter (even a temporary one):** increment on the suspected collision path (or count `produced − consumed` reaching the channel depth), expose via GETSTATS. If it tracks our ~400 events/3 h and the per-event ~12 buffers, hypothesis confirmed.
- **A byte-granular produced counter (`BYTE_COUNT`)** instead of `glDMACount × 16 KB`: removes our partial-buffer rounding and tells us the *exact* lost-byte count per event.
- **Forced near-boundary commits:** if you can phase the commit deliberately into the danger band vs. mid-buffer in a bench test, you should be able to *induce* or *avoid* the loss on demand — the cleanest possible confirmation.

## Two forward paths

**A. Fix the current approach — boundary-aware commit (Q5).** Keep the in-band marker but never force a commit inside the danger band; defer + record the offset. This is the lever our data points at.

**B. Sidestep it — capture, don't commit (the strong alternative).** Have the GPIF **snapshot the sample/byte counter into a register on the CTL[2] edge** without forcing any commit, and expose it via EP0 for the host to read (poll ~1 Hz). This gives sample-exact edge timing with **zero stream perturbation** — no partial commit, so no boundary collision possible. Given that two independent findings now point away from the forced-commit path, this may be the better long-term design. Open questions: can the GPIF counter be captured to a CPU-readable register on a CTL edge in this SDK, and what's its resolution?

## Lower priority, high value regardless (measurement hardening)

- **#1 drop/overflow event counter** in GETSTATS — makes host loss detection exact, retires our `INDETERMINATE` guard.
- **#2 sample-granular produced counter** — drops our loss resolution from 65,536 samples to ~1 and closes the produced-vs-delivered rounding residual.
