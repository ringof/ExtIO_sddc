# PPS in-band marker — firmware-side answers

Responding to the rx888-tools investigation request. All findings
are from the firmware source, SDK headers (`SDK/fw_lib/1_3_4/`),
and the GPIF II waveform (`SDDC_GPIF_PPS.h`).

## Q1: What does PPS_COMMIT actually do at the socket level?

**Nothing explicit.** The GPIF waveform never commands a buffer
wrapup.

The SDK defines `CYU3GPIF_BETA_THR_WRAPUP = 0x10000000` (bit 28
of the beta outputs / word 2 of the waveform descriptor) as
"Shut-down thread and wrap up buffer." **This bit is not set in
any state in the entire waveform** — not in PPS_COMMIT, not in
EVENT, not in BUSY, not anywhere.

The actual PPS flow (decoded from the waveform descriptors and
confirmed against the SDK beta enum in `cyu3gpif.h:207-241`):

```
TH0_RD ──(CTL[2] toggle)──► TH0_EVENT ──► TH0_PPS_COMMIT ──► TH1_RD_LD
```

- **TH0_RD** transition B (→ TH0_EVENT): beta = `POP_RQ` (0x40).
  Pops sampled data from read queue. No commit, no wrapup.

- **TH0_EVENT** transition B (→ TH0_PPS_COMMIT): beta =
  `POP_RQ | UPD_AOUT` (0x100040). Pops data, updates address
  output. No commit, no wrapup.

- **TH0_PPS_COMMIT** transition A (→ TH1_RD_LD): beta =
  `PUSH_WQ` (0x80). Pushes data into write queue (same action as
  normal TH0_RD left transition). No commit, no wrapup.

**TH0_PPS_COMMIT's waveform descriptor is byte-identical to
TH0_RD's left (DATA_CNT_HIT) transition.** It does exactly what
the normal buffer-full path does: read one sample, write it, and
transition to the other thread's RD_LD. The only difference is
that TH0_PPS_COMMIT transitions *unconditionally* (no
DATA_CNT_HIT required), leaving the current buffer partially
filled.

TH1_PPS_COMMIT (state 13) uses position table entry 1, mapping to
the **IDLE** waveform descriptor. IDLE has no data actions — it
just waits. So TH1_PPS_COMMIT is effectively a no-op pass-through
to TH0_RD_LD.

**The short transfer must be produced by an implicit DMA mechanism
when the GPIF switches threads** — possibly the DMA adapter
auto-wrapping the abandoned socket's buffer, or the socket
detecting that writes stopped. The GPIF is not commanding a
wrapup. This means the partial buffer commit is a *side effect*
of the thread switch, not a deliberate action.

## Q2: Buffer topology (confirmed)

From `StartStopApplication.c:159-172` and `Application.h:55-58`:

```
DMA_BUFFER_SIZE    = 16 KB (16 × 1024)
DMA_BUFFER_COUNT   = 4
Channel type       = CY_U3P_DMA_TYPE_AUTO_MANY_TO_ONE
Transfer size      = 0 (infinite)
DMA mode           = CY_U3P_DMA_MODE_BYTE

Producer sockets:
  [0] = CY_U3P_PIB_SOCKET_0  (GPIF Thread 0)
  [1] = CY_U3P_PIB_SOCKET_1  (GPIF Thread 1)

Consumer socket:
  [0] = CY_U3P_UIB_SOCKET_CONS_1  (USB EP1 IN)
```

4 buffers total in the channel, shared across 2 producer sockets
and 1 consumer socket. The channel is AUTO — the DMA adapter
forwards buffers from producers to consumer without CPU
involvement.

DATA_COUNT_LIMIT = 0x1FFE = 8190 (16-bit words). Combined with
the 1 sample read in RD_LD, that's 8191 words = 16382 bytes per
buffer fill cycle. DMA_BUFFER_SIZE is 16384 bytes. There's a
2-byte discrepancy — either a pipeline detail or a rounding
artifact in the counter.

## Q3: What happens when PPS coincides with a buffer boundary?

This is where the implicit-commit mechanism becomes dangerous.

In normal operation:
1. DATA_CNT_HIT fires → GPIF transitions TH0_RD → TH1_RD_LD
2. The buffer is full → DMA auto-commits it (buffer-full trigger)
3. DMA loads next descriptor for TH0's socket
4. Thread switch is clean because the commit is synchronous with
   the buffer being full

In PPS operation:
1. CTL[2] toggle fires → GPIF transitions TH0_RD → TH0_EVENT →
   TH0_PPS_COMMIT → TH1_RD_LD
2. The buffer is NOT full → commit must happen implicitly
3. The implicit commit races with whatever the DMA adapter is doing

**Near-boundary collision scenarios:**

**Near-full (DATA_CNT_HIT about to fire):** The CTL[2] toggle and
DATA_CNT_HIT can fire on the same or adjacent clock cycles. The
GPIF has two transitions per state (left and right). If both
conditions are true simultaneously, which transition wins?
TH0_RD's left = DATA_CNT_HIT → TH1_RD_LD (normal full buffer).
TH0_RD's right = CTL[2] toggle → TH0_EVENT (PPS path). Priority
depends on the GPIF's transition arbitration, which is not
documented in the waveform — it's silicon behavior.

If the PPS path wins but the buffer is 1-2 samples from full, the
implicit commit fires on a nearly-full buffer at the same time the
DMA would naturally commit it. Double-commit or descriptor
confusion.

**Near-empty (buffer just started):** The GPIF just switched to a
fresh buffer via RD_LD. CTL[2] toggle fires immediately. The GPIF
takes the PPS path, switching threads again. The buffer has ~1
sample in it. The implicit commit fires on a nearly-empty buffer.
Meanwhile, the DMA may not have finished the descriptor load for
this socket from the previous cycle (TRM page 70: descriptor
load = 3 bus transactions). The implicit commit could race with
the descriptor chain setup.

Both scenarios produce the "structural collision" consistent with
the 26× boundary enrichment in the brother's data.

## Q4: The ~12-buffer quantum

12 × 16 KB = 192 KB. With 4 buffers in the channel:
- 12 = 3× the total buffer pool
- 12 = 6× one producer socket's share (if evenly split: 2 per
  socket)

I don't have a definitive explanation. Possibilities:

- If the collision orphans all buffers currently in-flight between
  the producer commit and the USB consumer drain, and the USB
  consumer batches at a granularity related to the endpoint burst
  configuration (ENDPOINT_BURST_LENGTH = 16 packets × 1024 bytes =
  16 KB = 1 DMA buffer), then 12 might represent the USB-side
  in-flight queue depth at the moment of collision.

- The USB host (librx888) submits multiple URBs. Each URB is
  `req_packets × max_packet` = 1 MB. 12 DMA buffers = 192 KB,
  which is ~18.75% of one URB. This doesn't map cleanly.

- 12 could be the DMA adapter's internal descriptor ring depth
  for AUTO_MANY_TO_ONE channels. This would need the TRM's DMA
  adapter internals chapter.

**The 12-buffer quantum is the strongest clue.** Whatever
mechanism produces exactly 12 lost buffers per event names the
failure path. This needs DMA adapter documentation beyond what's
in the current TRM extract.

## Q5: Can the commit be made boundary-aware?

**Yes, partially.** The GPIF state machine has access to the data
counter value as a lambda input (`DATA_CNT_HIT` fires when the
counter reaches its limit). However, there's no lambda for
"counter is near limit" or "counter is in range X-Y."

What IS available:
- `DATA_CNT_HIT` — counter reached limit (buffer full)
- The counter value itself isn't directly readable as a transition
  condition (only the hit flag is a lambda)
- `ADDR_COUNT` — a separate counter, currently configured
  (ADDR_COUNT_CONFIG = 0x10A) but with limit 0xFFFF (effectively
  unused)

**Possible approach:** Use the address counter as a "danger zone"
detector. Configure it to count up alongside the data counter but
with a limit set to (8190 - danger_band). When ADDR_CNT_HIT
fires, the GPIF enters a "PPS-inhibit" zone where CTL[2] toggle
is ignored (transition disabled). After DATA_CNT_HIT fires
(buffer completes normally), the inhibit clears.

This requires redesigning the state machine to add inhibit states
and split the CTL[2] transition into "safe" and "inhibited"
variants. It's doable in the GPIF Designer but adds ~4-6 states.

The deferred PPS would record the inhibited-cycle count so the
host can recover the true PPS position: "PPS was at sample
(buffer_boundary + deferred_offset)."

## Q6: Is the failure truly silent?

**Yes, from GETSTATS.** The GETSTATS response
(`USBHandler.c:257-301`) exposes:

| Offset | Field | Tracks PPS loss? |
|---|---|---|
| 0-3 | glDMACount | Producer-side only |
| 4 | gpifState | Instantaneous, not historical |
| 5-8 | glCounter[0] | PIB error count |
| 9-10 | glLastPibArg | Last PIB error argument |
| 11-14 | glCounter[1] | PIB error detail |
| 15-18 | glCounter[2] | PIB error detail |
| 19 | Si5351 status | Clock, not DMA |
| 20-23 | boot count | Reset detection |
| 24 | CLK0 register | Clock, not DMA |
| 25 | clk0_enabled | Clock, not DMA |
| 26-29 | glPpsCount | **Always 0** — only incremented by synth_pps.c, never runs in PPS_CTL_ENABLE=1 build |
| 30-33 | glPpsCommitFailCount | **Always 0** — same reason |
| 34-35 | glPpsLastWrapS0/S1 | **Always 0xFF** — same reason |

There is **no consumer-side counter**. No DMA channel error
counter. No descriptor-wrap or orphan-buffer counter. The DMA
callback only registers `CY_U3P_DMA_CB_PROD_EVENT` — consumer
events (`CY_U3P_DMA_CB_CONS_EVENT`) are commented out
(`StartStopApplication.c:166`).

**The entire consumer/drain side of the DMA channel is invisible
to firmware instrumentation.**

### Additional silent-failure note: glPpsCount is dead

In the `PPS_CTL_ENABLE=1` build, the PPS is handled entirely by
the GPIF hardware. `synth_pps.c` is never invoked — no timer, no
`SetWrapUp`, no counter increments. The `glPpsCount`,
`glPpsCommitFailCount`, and `glPpsLastWrapS0/S1` fields in
GETSTATS are vestigial — they report on a code path that doesn't
run.

There is currently **no firmware-side counter for GPIF-driven PPS
events.**

## Recommended instrumentation (firmware-side)

### 1. Consumer event counter (high priority)

Enable `CY_U3P_DMA_CB_CONS_EVENT` in the DMA config (currently
commented out at line 166). Add a `glDMAConsCount` counter. The
difference `glDMACount - glDMAConsCount` is the instantaneous
in-flight buffer count. If this grows monotonically during PPS
events, it directly confirms the orphan hypothesis.

```c
dmaMultiConfig.notification = CY_U3P_DMA_CB_PROD_EVENT
                            | CY_U3P_DMA_CB_CONS_EVENT;
```

```c
if (type == CY_U3P_DMA_CB_PROD_EVENT)
    glDMACount++;
else if (type == CY_U3P_DMA_CB_CONS_EVENT)
    glDMAConsCount++;
```

Expose `glDMAConsCount` in GETSTATS. If
`glDMACount - glDMAConsCount` jumps by ~12 at each loss event and
never recovers, that's the smoking gun.

### 2. GPIF PPS event counter

The GPIF beta bit `INTR_CPU` (0x00040000, bit 18) can be added to
the TH0_EVENT or TH1_EVENT waveform descriptors. This fires the
DMA callback (same as TH0_BUSY/TH1_BUSY do for normal buffer
completions). A counter in the callback for "interrupt from EVENT
state" would count GPIF-driven PPS events — replacing the dead
`glPpsCount`.

This requires a waveform descriptor edit (set bit 18 in the EVENT
states' word 2) and a way to distinguish EVENT interrupts from
BUSY interrupts in the callback. The callback receives
`CyU3PDmaCBInput_t *input` which may carry enough context. Needs
verification.

### 3. Boundary-collision detector

If the data counter value is readable from CPU context (via the
`CY_U3P_PIB_GPIF_DATA_COUNT_LIMIT` register or a related status
register), the PPS event interrupt (from #2 above) could snapshot
the counter value. If the snapshot shows values near 0 or near
8190 correlating with loss events, that directly confirms the
boundary-collision mechanism.

## The two forward paths (firmware perspective)

### A. Boundary-aware commit

Add GPIF states that defer the PPS commit out of the danger band.
Use the address counter as a zone detector. ~4-6 new states in the
waveform. The GPIF Designer should handle this, but the manually-
patched header complicates iteration — the XML model doesn't have
the PPS states at all (they were hand-added to the generated
header). Ideally, rebuild the PPS waveform in the Designer from
scratch with the boundary-aware logic.

### B. Capture, don't commit (MCU-side latch)

Configure GPIO 19 as input with `CY_U3P_GPIO_INTR_POS_EDGE`
(production build, `PPS_CTL_ENABLE=0`). GPIO ISR latches
`glDMACount`. Expose via GETSTATS. Zero stream perturbation,
buffer-level resolution (~63 µs at 129.6 MSPS). Uses stock GPIF
waveform — no PPS states needed.

The latch is architecturally simpler and immune to the boundary
collision, but gives up sample-exact resolution. See
`pps_alternative.md` for the full tradeoff analysis.

### Which path?

The instrumentation from recommendations 1-3 should come first
regardless of path. It makes the failure visible, confirms or
kills the boundary-collision hypothesis, and provides the
telemetry needed to validate either fix.

If the boundary-collision hypothesis is confirmed and the
boundary-aware commit (path A) can be implemented cleanly in the
GPIF Designer, that's the superior result — sample-exact, zero
data loss, all hardware. If the waveform complexity is
unmanageable or the collision has additional failure modes beyond
the boundary race, path B is the clean fallback.
