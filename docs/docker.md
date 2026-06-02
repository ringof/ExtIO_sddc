# docs/docker.md — ka9q-radio container: lessons paid for

The container's design is **exec in, data out**: tools (`radiod`, `powers`,
`control`, `lsusb`, `pkill`, etc.) are invoked via `docker exec` from the
host or test harness, and structured output (logs, CSVs, exit codes) is
captured back. That is the intended interface — don't fight it.

What this doc captures: the *specific* gotchas in that mode that bit us
during the SHDN soak validation, written so the next investigator (or the
next session) doesn't re-pay for them. Each section corresponds to time
we burned tracking down something that was, in hindsight, an environment
quirk and not a firmware/radiod bug.

---

## 1. `docker exec` has three meaningfully different forms

Pick the form that matches the use case; mixing them is how bugs are born.

| Form | Use for | Key properties |
|---|---|---|
| `docker exec -it CONTAINER bash` | Interactive hand-debugging | Has TTY, forwards SIGINT, stdio line-buffered, signals work |
| `docker exec CONTAINER cmd args` | One-shot tool capture | No TTY, stdout captured raw; SIGINT in host shell does **not** forward to the in-container process |
| `docker exec CONTAINER sh -c "cmd ..."` | Compound commands, redirects, backgrounding | No TTY, stdio block-buffered when piped, dash as sh |

Concrete things that bit us:

- **`Ctrl-C` does not forward to a non-TTY `docker exec`.** Our shell
  returns, but the in-container `radiod` keeps running, holds the device,
  and the next start fails. To signal a process inside the container,
  use `docker exec CONTAINER pkill -INT -x procname` — that's a real
  in-container SIGINT.
- **Don't blame the invocation form for downstream parsing bugs.** A
  long detour during the SHDN soak attributed harness-vs-interactive
  divergence (`Invalid response, length 0` in the harness, instant CSV
  by hand) to TTY / process-group / session differences between
  `docker exec sh -c "..."` and `docker exec -it bash`. The actual root
  cause was an `awk -F,` field-separator bug in `capture_spectrum`: the
  CSV was *being captured* by the harness identically in both forms;
  the parser was throwing it away because powers writes `", "` between
  fields and the validity regex didn't allow the leading space. The
  misleading TAP message `powers returned no spectrum despite radiod
  READY` then sent us hunting through invocation layers when the real
  bug was 5 lines downstream. Before suspecting the invocation form,
  print the captured value and exercise the parser by hand.
- **Stdout buffering changes when stdout is not a TTY.** libc switches
  block-buffered (4 KB) for piped/redirected stdout. A tool that prints
  one short line then exits may produce no observable output before a
  `timeout` kills it. Wrap with `stdbuf -oL TOOL ...` (or have the tool
  flush) when capturing.

## 2. Host networking + pin multicast to loopback — interface selection matters

The container uses **`--network host`** (not bridge). This reverses an
earlier decision; the reason is §1: radiod's cold-start re-acquire after a
firmware upload is a USB **hotplug** event, hotplug arrives over a
**network-namespace-scoped** netlink socket, and a bridge container has its
own netns and never hears it. Host netns is the only way libusb sees the
`00f3`→`00f1` re-enumeration, so any harness that exercises cold start needs
it. (Bridge *would* work for a flow that only ever hot-starts — e.g. one that
pre-loads firmware host-side — but we run host net uniformly for one model.)

Bridge networking was originally chosen to avoid the multi-homed-host hazard
(producer and consumer landing on different host interfaces). Under host net
that hazard is real again, so we neutralize it the same way ka9q itself does —
**keep everything on loopback (`lo`)**:

- **radiod sends status/data on `lo`** — it logs the choice at startup:

  ```
  Multicast enabled on loopback interface lo
  ```
- **A consumer (`powers`, `monitor`, `control`) without an explicit interface
  joins on whatever interface the kernel's routing picks** — typically the
  default-route NIC, *not* `lo`. Send-side on `lo`, recv-side elsewhere → they
  never meet. So consumers must be pinned.
- ka9q's `resolve_mcast` (`src/multicast.c`) parses a `,iface` suffix on group
  names: `hf.local,lo` pins the join to `lo`. The harness sets `SPEC_IFACE=lo`
  by default and appends `,lo` to the group name passed to `powers`; the
  `-I lo` defaults in `ka9q_smoke.sh`/`hf_sweep.sh` do the same.

So the determinism is unchanged in practice — everything stays on `lo` — but
it's now a property of **pinning** (radiod's default + the consumer `,lo`
hint), not of network-namespace isolation. The multi-homed hazard only bites
an **un-pinned** consumer; the harness pins. If you add a new consumer of a
status group, pin its iface explicitly — don't assume routing picks `lo`.

> If radiod ever logs an interface other than `lo` on your host, pin the
> consumers to match it (`SPEC_IFACE=<iface>` / `-I <iface>`). The `,lo`
> default assumes radiod's loopback default holds; confirm with the smoke test.

## 3. avahi-daemon in this image — do not bounce it

`entrypoint.sh` starts `dbus-daemon --system &`, sleeps 0.5s, then
`avahi-daemon --no-drop-root --daemonize 2>/dev/null || true`. With
`--daemonize`, avahi double-forks: the original process exits, the
daemon detaches as a child of init. **PID 1 is responsible for reaping
the intermediate parent.**

When the harness overrides CMD with `sleep infinity`, **PID 1 is `sleep`,
which does not reap children.** Combined with the `|| true` swallowing
errors silently, this means a `pkill avahi-daemon; avahi-daemon
--daemonize --no-drop-root` sequence (intended to clear stale records
between cycles) produces:

- one or more **`<defunct>` avahi-daemon zombies** (the daemonize
  parents that nobody reaped), plus
- one alive avahi-daemon whose D-Bus interface is **wedged**.

The wedged daemon is sneaky because **`avahi-daemon --check` returns 0**
(it only checks process existence, not functionality). The observable
symptom is `avahi-resolve -n hf.local` returning `Failed to resolve host
name 'hf.local': Timeout reached` — and `getent hosts hf.local` timing
out the same way, because both ultimately depend on the daemon. We
manufactured exactly this in the "harden the avahi reset" change and
then chased the resulting failures as if they were a different problem.

**Rule:** do not kill/respawn avahi-daemon during the test. A clean
radiod SIGINT triggers a D-Bus disconnect that makes avahi auto-tear-down
that client's entry groups (including `hf.local`). If a stale name
lingers, log a warning; do not touch the daemon.

If you must restart avahi in this image, also fix PID 1 to reap zombies
(`tini` or similar as an init wrapper).

## 4. Two `.local` resolver paths exist and they disagree

For `*.local` names, there are **two independent paths**:

| Path | Used by | Mechanism |
|---|---|---|
| `avahi-resolve -n NAME` | direct callers | D-Bus → avahi-daemon's local record store |
| `getaddrinfo()` / `getent hosts NAME` | ka9q tools (via `resolve_mcast`), libc-using programs | NSS → nss-mdns → mDNS multicast query → wait for response |

These can disagree. `avahi-resolve` succeeds the moment a record is in
avahi's store; `getaddrinfo` requires the daemon's mDNS responder to be
**announced and answering on multicast** — that takes longer, can race
with avahi's probe phase, and can break independently of D-Bus (see
Section 3).

**Rule:** if a check is going to gate a consumer that uses
`getaddrinfo` (every ka9q tool), do not use `avahi-resolve` as the
readiness gate. Use the *same* resolver path the consumer will use
(`getent hosts NAME` is the closest cheap proxy).

## 5. `resolve_mcast` is core ka9q architecture — don't replace it

`src/multicast.c:274` (`resolve_mcast`) is the **single resolution path**
for every ka9q tool: radiod, control, monitor, powers, opusd, aprs,
jt-decoded, metadump, monitor-data. It is `getaddrinfo`-based by design:

- Portable across Linux/BSD/macOS without an avahi/D-Bus dependency.
- Supports `/etc/hosts` static fallback (useful in stripped-down
  environments).
- Retries forever when called with `tries=0` (no give-up, no backoff —
  if the resolver eventually answers, the tool eventually proceeds).
- Parses `name,iface` suffix to pin the multicast interface.

**Do not propose changing `resolve_mcast` to use avahi D-Bus directly.**
That breaks the portability guarantee Phil Karn (rightly) protects.
The supported levers when resolution is fragile are: pin the iface via
`,iface`, fix the environment (nss-mdns config, `/etc/hosts`), or work
on whatever is making the daemon's mDNS responder unreliable.

## 6. USB device — clean stop vs dirty stop, and the claim race

- **SIGINT** to radiod runs `rx888_stop_rx`: drains in-flight URBs,
  sends `STOPFX3`, releases the libusb interface, closes the handle.
  Next radiod claims immediately. Verified clean across many cycles in
  manual hand-testing.
- **SIGKILL** to a *streaming* radiod skips that drain. The kernel
  asynchronously tears down the dead process's usbfs claim — which takes
  some milliseconds to seconds. During that window, the next radiod's
  `libusb_claim_interface` returns `BUSY` and `rx888_usb_init` bails
  immediately with `Error claiming USB interface` /
  `device setup returned -1`. **Retry once and it succeeds** — the race
  resolves itself within a second.

This is a real ka9q robustness gap: `rx888_usb_init` doesn't retry the
claim on `EBUSY`. A short retry loop (claim, on busy/error sleep ~100 ms,
try a handful of times) would absorb the post-kill teardown window. This
is a clean upstream candidate, alongside the existing `04-no-tuner-stdby`
patch.

The firmware does not need to do anything different — it is fully
recoverable from a SIGKILL of the streaming host, with or without a
clean `STOPFX3` having been sent.

## 7. Reused containers accumulate state — start clean

The harness reuses a running container by default (logs `reusing running
container 'ka9q-radio-soak'`). When prior runs were interrupted (Ctrl-C
mid-cycle, hooks not fully completing cleanup, etc.), the container can
contain:

- a still-alive radiod holding the USB device,
- a stale `hf.local` registration in avahi,
- a bound multicast socket → `Address already in use` on the next start.

A "polluted" container manufactures problems that look like firmware or
radiod bugs. If results look weird and you cannot explain them by reading
the script's actual actions, **kill and recreate the container before
spending an hour on theory:**

```bash
docker rm -f ka9q-radio-soak
```

This was responsible for a meaningful fraction of the "intermittent"
behavior in the SHDN soak work.

## 8. Readiness ≠ data-plane

Log strings like `rx888 running` and `Established under name 'hf.local'`
are necessary but not sufficient for "consumer can actually pull data."
Specifically:

- `Established under name 'hf.local'` can be logged even after
  `Failed to add service: Local name collision` — radiod retries
  registration, eventually a variant takes, and the log says
  `Established` whether the original name or a renamed one (`hf-2.local`)
  is what survived.
- A multicast group name resolving via `avahi-resolve` does not mean the
  data plane is reachable from the consumer's interface (Section 2).
- A multicast group name resolving via `avahi-resolve` does not mean it
  resolves via `getaddrinfo` (Section 4).

A "truer" readiness gate actually issues the data-plane request the
consumer would make (e.g., a short `powers` pull) instead of log-grepping
+ one resolver path. The harness's `wait_radiod_ready` evolved through
several iterations of this lesson; if you change it, make sure the gate
matches what the *consumer* needs, not what's easy to grep.

## 9. The harness is under test, not above it

A pattern that ate hours: when manual cycling in `docker exec -it bash`
works and the harness fails, **the harness is the variable** — but
investigate the *whole* harness path (invocation, capture, parsing,
classification), not just the parts that look like environment quirks.
The headline lesson from the SHDN soak:

- The harness fired `powers` correctly, the CSV came back correctly,
  and an `awk -F,` field-separator bug in `capture_spectrum` threw it
  away because powers writes `", "` between fields. The TAP message
  `powers returned no spectrum despite radiod READY` then sent the
  investigation chasing avahi, multicast interfaces, `docker exec`
  forms, TTYs, process groups, and settle pauses — all wrong layers.
- The right move on the *first* `no spectrum` failure would have been
  to capture the raw `$csv` value and run the parser against it. That
  one probe would have located the bug in minutes.

**Rule:** when you have manual evidence that a flow works and the
harness fails to reproduce it, the next investigation is the harness
itself — strip its wrapping layer by layer **and inspect what each
layer actually produces vs. consumes** until you find where the value
disappears. A misleading error message naming a downstream effect
(`powers returned no spectrum`) is not evidence about which layer is
broken. Do not propose patches to radiod/avahi/firmware on the theory
that the harness is just observing a real bug; the harness
may be *causing* the bug it's claiming to observe.
