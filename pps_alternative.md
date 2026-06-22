# PPS marker approaches — analysis and status

## Current in-band mechanism (GPIF PPS commit)

The current in-band PPS marker (issue #125) uses the GPIF state
machine to detect a rising edge on CTL[2] (GPIO 19) and transition
into dedicated `TH0_PPS_COMMIT` / `TH1_PPS_COMMIT` states
(`SDDC_GPIF_PPS.h`, states 12 and 13). These states commit the
partially-filled DMA buffer, producing a short USB bulk transfer as
a 1 Hz delimiter. After the commit, the GPIF cross-routes to the
other thread's RD_LD state and continues filling the next buffer.

This is a hardware-level mechanism — the GPIF control comparator
(mask `0x00000004` = CTL[2]) triggers the state transition without
CPU involvement. The `synth_pps.c` module (`SetWrapUp`) is a
separate software-driven path used only for synthetic testing when
`PPS_CTL_ENABLE=0`.

### Why GPIF in-band is the better time-propagation source

The GPIF in-band marker is architecturally superior to EP0
request-response for time propagation:

- **Sample-exact, self-consistent.** The PPS edge causes an
  immediate GPIF state transition (1–2 ADC clocks). The short
  transfer boundary appears inline in the bulk data stream — the
  host knows exactly which sample was the last before the PPS edge.
  The timestamp IS the data. No cross-path correlation needed.

- **All-hardware path.** GPIF → DMA → USB bulk. No CPU, no polling,
  no OS scheduling jitter.

- **No correlation problem.** EP0 latch requires the host to match
  a GETSTATS response (traveling the EP0 control path) with a
  position in the bulk stream (traveling EP1) — two different USB
  paths, two different latency profiles. The in-band marker has no
  such ambiguity.

- **Resolution.** ~7.7 ns at 129.6 MSPS (one ADC clock) vs ~63 µs
  buffer-level (EP0 latch). 3–4 orders of magnitude.

This is why fixing the in-band approach is worth pursuing rather
than retreating to EP0.

## Observed data loss — current status

### The evidence

Controlled experiments at 129.6 MSPS (rx888-tools pps_integrity /
stream_soak):

| Run | Rate | Throughput | Short xfers | Loss |
|---|---|---|---|---|
| stream_soak (no marker) | 129.6 MSPS | 259 MB/s | 0 | 0 |
| pps_integrity (1 Hz marker) | 129 MSPS | 258 MB/s | 333 | ~82 MB |

The bare stream sustains 259 MB/s for 3 hours with zero short
transfers and zero loss (produced == delivered to within 16 KB).
The lossy run was at *lower* throughput with markers, while the
*higher* throughput without markers was clean. This inversion
exonerates the streaming path — if it were a drain limit, the higher
bare rate would fail first. It didn't.

### The loss is anomalously large

A correct forced-commit should cost 0–2 samples: it ships a partial
buffer early and starts a fresh one. The observed loss is ~500×
larger:

- ~82 MB lost across ~333 events over 3 h → ~250 KB per event
- At 259 MB/s that's ~0.5–1 ms of stream gone per event
- Expected: 1–2 ADC clocks (~15 ns). Observed: ~500 µs–1 ms.

This is not "marking costs a little." This is a state-machine or
DMA stall causing ~500× more loss than the intrinsic cost of a
buffer commit. **The implementation has a pathology; the concept
is not yet proven broken.**

### Suspected root causes

Two uncontrolled variables in the current test rig:

**1. Edge quality (prime suspect).** The GPIO 18→19 loopback uses
a 100 kΩ resistor, giving an RC edge of ~4 µs. At 129.6 MSPS
that's ~500 clock cycles dwelling in the threshold region. A slow
edge into a sampling element is the classic recipe for
metastability — and metastability resolving into a bad GPIF state
is exactly the kind of thing that produces a ~0.5 ms stall on
~5% of edges.

**2. Input synchronization.** The PPS signal on CTL[2] is
asynchronous to the ADC clock. The GPIF samples it on the clock
edge, but a single sample of an asynchronous signal is not a
synchronizer — the first flop can latch a metastable value and
propagate it into the state machine. The standard fix is a two-flop
(double-FF) synchronizer: FF1 may go metastable, FF2 resolves it
before it reaches logic. **Open question:** does the FX3 GPIF II
synchronize CTL inputs internally before they drive state
transitions? If not, we need a synchronizer.

### DMA descriptor chain behavior (TRM finding)

The FX3 TRM (page 70) confirms that the DMA descriptor chain
advance is not instantaneous. After a buffer commit, the producer
must:

1. Write descriptor back to memory (DMA-to-memory write)
2. Send produce event to consumer socket (DMA-to-MMIO write)
3. Load next descriptor from memory (DMA-to-memory read)
4. If next buffer occupied → socket stalls

`DMA_RDY` deasserts during steps 1–3. This is structural — even
with pre-queued buffers, the descriptor load cost is paid every
time. The two-thread ping-pong exists to hide this latency: while
TH0's socket does the descriptor swap, the GPIF cross-routes to
TH1 whose socket is already active.

This means the PPS cross-route is timing-sensitive: if TH1 is
mid-descriptor-swap when the PPS fires, the GPIF lands in
TH1_BUSY/WAIT and stalls for the full swap duration — losing every
sample during that window. At 129.6 MSPS, buffers fill in ~63 µs,
making the collision window a non-trivial fraction.

## Experiment plan

### Step 1: Matched-rate baseline (current rig)

`pps_integrity 3 --rate 129` and `stream_soak 3 --rate 129` — same
rate, same duration. Confirms the effect at the production rate,
removes the 129-vs-129.6 mismatch. This measures the *current rig*,
not the concept.

### Step 2: Clean edge (resistor change or logic buffer)

Replace the 100 kΩ with a fast edge (<1 ADC clock transition).
Options: lower resistor value, or a logic buffer (74LVC1G17 Schmitt
trigger). Re-run `pps_integrity 3 --rate 129`.

If loss drops sharply → edge quality was a major cause.

### Step 3: Input synchronization

Determine whether the FX3 GPIF II synchronizes CTL inputs
internally. If not, add a 2-FF synchronizer on the PPS input
(either in a CPLD/buffer external to the FX3, or by adding
synchronizer states in the GPIF waveform). Re-run.

If loss drops to ~0–2 samples/marker → in-band marking is viable
for production.

### Determination

If after steps 2+3 the per-marker cost lands at the expected 1–2
samples, then the "demote PPS off the data path" conclusion was
premature — it was the rig, not the idea. The 500× overshoot in
per-event loss strongly suggests a fixable artifact (metastability,
synchronization), not an intrinsic cost.

## Alternative: MCU-side latch (out-of-band)

### Role

The latch is a **diagnostic and characterization tool**, not a
replacement for in-band marking. It answers "did the PPS arrive?"
and "roughly where in the stream?" without perturbing the data —
useful for PPS integrity testing and timing studies where the
measurement shouldn't affect the thing being measured.

If the in-band approach proves unfixable (steps 2+3 don't resolve
the loss), the latch becomes the fallback — but with the
understanding that it trades 3–4 orders of magnitude of time
resolution.

### Feasibility

**Yes, straightforwardly.** Three pieces:

1. **GPIO interrupt support exists.** The FX3 SDK provides
   `CY_U3P_GPIO_INTR_POS_EDGE` (and NEG/BOTH/LEVEL variants) via
   `CyU3PGpioSimpleConfig_t.intrMode`. The firmware currently sets
   `CY_U3P_GPIO_NO_INTR` everywhere (`RunApplication.c:76`,
   `StartUp.c:29`) — but that's a config choice, not a hardware
   limitation.

2. **`glDMACount` is trivially ISR-safe.** It's a plain `uint32_t`
   global (`StartStopApplication.c:17`), incremented in the DMA
   callback. On the FX3's ARM9, a 32-bit aligned read is atomic. The
   GPIO ISR just does `latched_count = glDMACount;` — no locks, no
   blocking calls.

3. **Callback registration is one SDK call.**
   `CyU3PRegisterGpioCallBack()` registers a function called on any
   GPIO interrupt. The ISR checks which pin fired, latches
   `glDMACount`, and returns. Lightweight enough for ISR context.

### Within-buffer byte offset — not worth pursuing

`glDMACount` alone gives buffer-level resolution (~63 µs at
129.6 MSPS). Finer resolution via DMA socket `BYTE_COUNT` registers
is theoretically possible but practically problematic:

- **Ping-pong ambiguity:** ISR must determine which of two producer
  sockets is actively filling — fiddly and error-prone.
- **Coherency risk:** No SDK guarantee that `BYTE_COUNT` is coherent
  mid-burst. False precision is worse than coarse-but-correct.
- **Marginal benefit:** Buffer-level already exceeds USB transport
  jitter.

Confirmed by rx888-tools side: the risks of errors outweigh the
benefit, and the resolution isn't necessary for the use cases.

### GPIO pin selection

- **GPIO 19 (BIAS_HF)** in production mode (`PPS_CTL_ENABLE=0`):
  reconfigure as input with `CY_U3P_GPIO_INTR_POS_EDGE`. The
  existing 100k resistor from GPIO 18 carries the pulse.
- In `PPS_CTL_ENABLE=1` mode: GPIO 19 is owned by the GPIF as
  CTL[2] — cannot register a GPIO interrupt.

## Comparison

| Axis | GPIF PPS commit | MCU-side latch |
|---|---|---|
| Time resolution | **~7.7 ns** (sample-exact at 129.6 MSPS) | ~63 µs (buffer-level) |
| Time propagation | **Self-consistent** — marker is inline in data stream | EP0 side-channel — host must correlate two USB paths |
| Stream perturbation | Partial buffer commit + cross-route | **Zero** — GPIF/DMA unmodified |
| Data integrity | Anomalous loss on current rig (~250 KB/event); root cause under investigation | **No risk** |
| CPU involvement | **None** — all GPIF hardware | GPIO ISR once/second (trivial) |
| Host complexity | Detect short transfers at wire speed | Poll GETSTATS at leisure |
| Status | Under investigation — edge quality and synchronization are open variables | Ready to implement |

## Bottom line

The in-band GPIF marker is the right architecture for production
time propagation — sample-exact, self-consistent, all-hardware. The
observed data loss is anomalously large (500× expected), consistent
with edge-quality and input-synchronization artifacts on the current
test rig, not with an intrinsic limitation of the commit mechanism.

The experiment sequence (matched baseline → clean edge → synchronizer)
will determine whether the loss is a fixable rig artifact or a
fundamental problem. The MCU-side latch serves as a diagnostic tool
and fallback, not as the primary time source.
