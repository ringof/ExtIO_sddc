# MCU-side PPS latch — FX3 feasibility analysis

## Problem

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

Evidence from pps_integrity testing suggests the GPIF PPS commit may
cause data loss — samples arriving during the commit/cross-route
transition may be dropped.

The rx888-tools side proposed an alternative: instead of perturbing
the stream with a GPIF state transition, latch the DMA position when
the GPIO edge arrives and expose it via GETSTATS. Zero perturbation,
out-of-band timestamping.

## Can the FX3 latch `glDMACount` on a GPIO interrupt?

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

## Which GPIO pin?

- **GPIO 18 (BIAS_VHF)** — currently the **output** the host
  toggles. For the synthetic test (GPIO 18→19 loopback through the
  100k resistor), GPIO 18 is the driver, not the listener.

- **GPIO 19 (BIAS_HF)** — the natural PPS input candidate:
  - In **production mode** (`PPS_CTL_ENABLE=0`): GPIO 19 is a simple
    GPIO output. It could be reconfigured as input with
    `CY_U3P_GPIO_INTR_POS_EDGE`. The existing 100k resistor from
    GPIO 18 would carry the synthetic PPS pulse. This works for the
    test harness.
  - In **PPS_CTL_ENABLE=1 mode**: GPIO 19 is released to the GPIF as
    CTL[2]. The GPIO block doesn't own it — you **cannot** register a
    GPIO interrupt on it. The GPIF sees the edge, but the GPIF is a
    fixed state machine — it commits the buffer rather than latching
    a counter.

The MCU-side latch works cleanly in **production GPIO mode** (the
default build). Switching from the GPIF PPS commit to the latch
means building without `PPS_CTL_ENABLE` and loading the original
GPIF waveform (no PPS_COMMIT states), while adding the GPIO ISR.

## Within-buffer byte offset

Not worth pursuing. `glDMACount` alone gives buffer-level resolution
(16 KB / (2 bytes/sample × rate) = **~125 µs at 64 MSPS**, **~63 µs
at 129.6 MSPS**), which is already better than USB transport jitter
and more than adequate for PPS characterization.

Finer resolution via DMA socket `BYTE_COUNT` registers is
theoretically possible but practically problematic:

- **Ping-pong ambiguity:** Two producer sockets
  (`CY_U3P_PIB_SOCKET_0`, `CY_U3P_PIB_SOCKET_1`). The ISR would
  need to determine which one is actively filling — fiddly and
  error-prone from interrupt context.
- **Coherency risk:** `BYTE_COUNT` is hardware-managed. No SDK
  guarantee that it's coherent mid-burst while the GPIF is actively
  writing. Reading a stale or transitional value would be worse than
  not reading it at all — it would inject false precision.
- **Marginal benefit:** Going from ~125 µs to sub-µs resolution
  doesn't change what you can do with the measurement. The
  buffer-level latch already answers "did the PPS arrive?" and
  "where in the stream was it?" to sufficient accuracy.

The risk of subtle errors outweighs the value of the extra
resolution. Stick with `glDMACount`.

## Comparison: GPIF PPS commit vs MCU-side latch

| Axis | GPIF PPS commit (current) | MCU-side latch |
|---|---|---|
| Stream perturbation | GPIF commits partial buffer, forces short transfer + cross-route | **Zero** — GPIF runs unmodified, every buffer fills completely |
| Data integrity | Deterministic, bounded loss during PPS_COMMIT→cross-route (1–2 GPIF clock cycles, needs empirical measurement) | **No risk** — never touches the DMA channel or GPIF state machine |
| Time resolution | **Sample-exact** — short transfer boundary marks exactly where in the stream the PPS edge landed | Buffer-level (~125 µs at 64 MSPS) — 3–4 orders of magnitude coarser |
| CPU involvement | **None** — GPIF hardware handles everything, zero CPU load during streaming | GPIO ISR fires once per second — trivial but nonzero |
| Detection mechanism | Short USB bulk transfer (host detects `nsamples < expected`) | GETSTATS field (host polls or reads post-hoc) |
| Host complexity | Detect short transfers in streaming path (in-band, must handle at wire speed) | Poll GETSTATS at 1 Hz alongside GPIO toggle (out-of-band, relaxed timing) |
| GPIF waveform | Custom `SDDC_GPIF_PPS.h` with 14 states including PPS_COMMIT + cross-route | Stock waveform (no PPS states needed) |

### Not "strictly better" — a genuine tradeoff

The latch eliminates stream perturbation and any risk of data loss,
but gives up sample-exact resolution. Whether that tradeoff is worth
it depends on the application:

- **PPS characterization / timing studies:** Buffer-level resolution
  is sufficient. The latch wins — zero perturbation means cleaner
  measurements and no concern about dropped samples corrupting the
  analysis.

- **Sample-exact timestamping for downstream SDR processing:** The
  GPIF commit wins — if the data loss is confirmed to be only 1–2
  samples (15–30 ns at 64 MSPS) once per second, that's negligible
  for virtually any receiver application, and the sample-exact
  boundary is genuinely valuable for precise time alignment.

The data loss question is empirically testable: stream at full rate
with PPS enabled, compare total samples received vs. expected from
the sample rate and elapsed time. If the deficit is consistently
1–2 samples per PPS edge, the GPIF approach is sound for production
use and the latch becomes a diagnostic/characterization tool rather
than a replacement.

## End-to-end operation

1. **Firmware:** Build without `PPS_CTL_ENABLE` (production GPIF
   waveform). Configure GPIO 19 as input with
   `CY_U3P_GPIO_INTR_POS_EDGE`. Register callback. On rising edge:
   `glPpsLatchedDMACount = glDMACount;` (optionally
   `glPpsLatchCount++`). Add both to GETSTATS response.

2. **Host (pps_integrity):** Each second, toggle GPIO 18 high (EP0
   GPIOFX3), wait 10 ms, toggle low. After each edge, read
   GETSTATS — compare `glPpsLatchedDMACount` against the running DMA
   count to verify the latch fired. No need to detect short transfers
   in the sample stream at all.

3. **Timing quality:** Host records `clock_gettime(CLOCK_REALTIME)`
   at each GPIO toggle. The latched DMA count gives the firmware-side
   timestamp in buffer units. The delta between host wall-clock
   intervals and DMA-count intervals reveals transport jitter — all
   without perturbing the stream.

## Bottom line

These are complementary mechanisms, not necessarily replacements:

- **GPIF PPS commit** gives sample-exact in-band markers with
  deterministic, bounded data loss (1–2 GPIF cycles, needs
  measurement). Best for production timestamping if the loss is
  confirmed negligible.

- **MCU-side latch** gives zero-perturbation out-of-band timestamps
  at buffer-level resolution. Best for PPS characterization,
  diagnostics, and timing studies where you don't want the
  measurement to affect the thing being measured.

Both could coexist — the latch requires `PPS_CTL_ENABLE=0` (GPIO 19
as GPIO, not GPIF CTL[2]), so it's a different firmware build, but
the two approaches serve different purposes and validate each other.
