# ka9q-radio patches applied in this container

These patches are applied during the Docker build (see Dockerfile)
against the pinned `KA9Q_RADIO_SHA` (currently `21d51fac`).  Each patch is a
deliberate ask of the upstream maintainer; the set is kept minimal so that
asks are focused on real, irreducible incompatibilities — not noise.

The Dockerfile's `COPY patches/*.patch` step is guarded against an empty
match, so the build also succeeds with zero patches applied.  Files with a
`.disabled` suffix are intentionally skipped by the `*.patch` glob.

## Active

| File | What it does | Why |
|------|-------------|-----|
| `04-no-tuner-stdby.patch` | `rx888.c`: drop both `command_send(...,TUNERSTDBY,0)` sends in `rx888_set_hf_mode()` and `rx888_start_rx()`. | The RX888mk2 is direct-sampling HF with no tuner; SDDC_FX3 has no `TUNERSTDBY` (0xB8) handler, so each send STALLs (`LIBUSB_ERROR_PIPE`). Previously judged cosmetic, but the restart soak (`ka9q_test.sh`) showed radiod's startup intermittently **wedging at the 0xB8 STALL** on a restart (gdb: main in `rx888_setup`, no streaming threads spawned). Removing the no-op sends eliminates the EP0 STALL on the HF path. **Still present in the new (`21d51fac`) host-side-Si5351 driver — both sends remain, identical context, so this patch applies clean.** Candidate upstream PR. |

## Disabled / historical

| File | What it was | Why it's disabled |
|------|-------------|-------------------|
| `01-powers-freq-double.patch.disabled` | `powers.c`: send `RADIO_FREQUENCY` via `encode_double` instead of `encode_float`. | **Upstreamed.** As of the pinned `21d51fac`, `powers.c` already does `encode_double(&bp,RADIO_FREQUENCY,frequency)` (line ~160) — exactly this fix. The patch no longer applies (context drift) and is no longer needed. Kept with `.disabled` suffix for archaeology. |
| `02-powers-rbw-float.patch.disabled` | `powers.c`: decode the response's `RESOLUTION_BW` with `decode_float` instead of `decode_double`. | **Upstreamed.** As of `21d51fac`, `powers.c`'s `extract_powers` already does `*rbw = decode_float(cp,optlen)` (line ~357) — exactly this fix. Receive-side mirror of `01`; same status. Kept for archaeology. |
| `03-startadc-before-startfx3.patch.disabled` | Inserted `command_send(...,STARTADC,samprate)` before `STARTFX3` in `rx888_start_rx` to wake the firmware's `glAdcClockEnabled` host-cache flag. | Superseded by SDDC_FX3 commit `f3835a5` on branch `claude/si5351-chip-query`: `si5351_clk0_enabled()` now reads CLK0_CONTROL reg 16 bit 7 from the Si5351 directly, so `GpifPreflightCheck()` sees the chip's real state when ka9q programs Si5351 via `I2CWFX3`.  No host-side workaround needed.  File kept in-tree with `.disabled` suffix (skipped by the `*.patch` glob) for archaeology. |

## Findings that did *not* become patches

The audit (`docs/ka9q-compat-audit.md`) identified a further ka9q-side
behavior that is observable but not blocking:

- **`sleep(1)` after firmware upload** in ka9q's `rx888_usb_init()`.
  Upstream's fixed 1 s wait is fragile in principle, but it is
  sufficient on every host we have tested when the container is run
  with `-v /run/udev:/run/udev:ro` (so libusb's hotplug listener sees
  fresh device events).  A polling loop would be more robust on
  pathologically slow hosts but is not required.  Documented in
  audit §1.

> **`TUNERSTDBY` (0xB8) on the HF path** was previously listed here as
> cosmetic. The restart soak reclassified it as blocking (intermittent
> startup wedge at the STALL) — now addressed by active patch
> `04-no-tuner-stdby.patch`. See audit §2.
