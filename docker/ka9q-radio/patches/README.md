# ka9q-radio patches applied in this container

These patches are applied during the Docker build (see Dockerfile)
against the pinned `KA9Q_RADIO_SHA` (currently `87567fa`).  Each patch is a
deliberate ask of the upstream maintainer; the set is kept minimal so that
asks are focused on real, irreducible incompatibilities — not noise.

The Dockerfile's `COPY patches/*.patch` step is guarded against an empty
match, so the build also succeeds with zero patches applied.  Files with a
`.disabled` suffix are intentionally skipped by the `*.patch` glob.

## Active

**None.** As of `87567fa` the container builds **vanilla ka9q-radio** — every
local compatibility ask has been upstreamed (see below).  This is the goal
state: no host-side patches needed for ka9q-radio to drive this firmware.

## Disabled / historical

| File | What it was | Why it's disabled |
|------|-------------|-------------------|
| `01-powers-freq-double.patch.disabled` | `powers.c`: send `RADIO_FREQUENCY` via `encode_double` instead of `encode_float`. | **Upstreamed.** `powers.c` already does `encode_double(&bp,RADIO_FREQUENCY,frequency)` (line ~160) — exactly this fix. Kept with `.disabled` suffix for archaeology. |
| `02-powers-rbw-float.patch.disabled` | `powers.c`: decode the response's `RESOLUTION_BW` with `decode_float` instead of `decode_double`. | **Upstreamed.** `powers.c`'s `extract_powers` already does `*rbw = decode_float(cp,optlen)` (line ~357) — exactly this fix. Receive-side mirror of `01`. Kept for archaeology. |
| `04-no-tuner-stdby.patch.disabled` | `rx888.c`: drop both `command_send(...,TUNERSTDBY,0)` sends in `rx888_set_hf_mode()` and `rx888_start_rx()`. | **Upstreamed at `87567fa`.** Phil removed the `rx888_start_rx` send and wrapped the `rx888_set_hf_mode` one in `#if 0 // not reimplemented yet in firmware` — exactly what this patch did. The patch no longer applies (context changed) and is no longer needed. The RX888mk2 is direct-sampling HF with no tuner; SDDC_FX3 has no `TUNERSTDBY` (0xB8) handler, so each send STALLed (`LIBUSB_ERROR_PIPE`) and the restart soak showed it intermittently wedging startup. See audit §2. Kept for archaeology. |
| `03-startadc-before-startfx3.patch.disabled` | Inserted `command_send(...,STARTADC,samprate)` before `STARTFX3` in `rx888_start_rx` to wake the firmware's `glAdcClockEnabled` host-cache flag. | Superseded by SDDC_FX3 commit `f3835a5` on branch `claude/si5351-chip-query`: `si5351_clk0_enabled()` now reads CLK0_CONTROL reg 16 bit 7 from the Si5351 directly, so `GpifPreflightCheck()` sees the chip's real state when ka9q programs Si5351 via `I2CWFX3`.  No host-side workaround needed.  Kept for archaeology. |

## Findings that did *not* become patches

The audit (`docs/ka9q-compat-audit.md`) identified ka9q-side behaviors that
are observable but not blocking:

- **`sleep(1)` after firmware upload** in ka9q's `rx888_usb_init()` — still
  present at `87567fa` (`// how long should this be?`). The actual cold-start
  failure we hit was a container netns/hotplug issue (fixed by `--network
  host`), not this sleep; on a real host the fixed wait is sufficient. A
  poll-with-timeout would be more robust on slow re-enumerators (e.g. a
  fiber-isolated USB link).  Documented in audit §1.
- **`TUNERSTDBY` (0xB8) on the HF path** — was an active patch (`04`); now
  **upstreamed** at `87567fa` (see above and audit §2).
