# ka9q-radio test environment for RX888mk2 firmware validation
#
# Build (from project root):
#          docker build -f docker/ka9q-radio/Dockerfile -t ka9q-radio .
# Run:     docker run --rm -it --privileged --network host \
#            -v /dev/bus/usb:/dev/bus/usb -v /run/udev:/run/udev:ro ka9q-radio
#
# --privileged: the FX3 changes USB PID during firmware upload (0x00F3 ->
# 0x00F1), which changes the device path.
# --network host: REQUIRED for radiod's in-container cold start. The post-upload
# re-enumeration (00f3->00f1) is a USB hotplug event, delivered over a
# netns-scoped netlink socket a bridge container can't hear, so libusb never
# sees the re-acquired device (docs/ka9q-compat-audit.md §1). Host netns fixes
# it. Multicast stays deterministic by keeping everything on loopback (radiod
# defaults to lo; consumers pin -I lo). Under host net ka9q-web binds host :8081
# directly — no -p publish needed.

FROM debian:bookworm-slim AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    ca-certificates \
    libavahi-client-dev \
    libbsd-dev \
    libfftw3-dev \
    libiniparser-dev \
    libncurses-dev \
    libopus-dev \
    libusb-1.0-0-dev \
    libusb-dev \
    uuid-dev \
    libogg-dev \
    libsamplerate-dev \
    portaudio19-dev \
    libgnutls28-dev \
    libgcrypt20-dev \
    && rm -rf /var/lib/apt/lists/*

# Clone ka9q-radio — only need radiod + rx888 plugin.
# Pin to a specific commit so audit results are reproducible. Bump
# this SHA when intentionally retesting against newer ka9q-radio.
#
# Pinned to b4388d87 (main, 2026-06-15). Host-side Si5351 rx888 driver
# (si5351.c), zero local patches — builds vanilla ka9q-radio.
#
# Key changes since the previous pin (87567fa):
#  - Watchdog: 5s timer resets only after a successful USB transfer (780882fa).
#  - Globals (usb_device_gone, Power_smooth, Reset) moved into struct sdrstate.
#  - isfinite() guarded with !isnan() to prevent float exceptions under debug.
#  - TESTFX3 firmware-query failure is now non-fatal (won't skip init).
#  - -march=native OFF by default (af511009); use `make NATIVE=1` to opt in.
#
# Still true (inherited from 87567fa / 21d51fac):
#  - Si5351 lock poll replaces blind sleep; TUNERSTDBY removed; firmware
#    version logged via TESTFX3; USB reset defaults OFF (config key `reset`).
#  - Cold start requires --network host (audit §1). See
#    docs/ka9q-compat-audit.md §12 and docker/ka9q-radio/patches/README.md.
ARG KA9Q_RADIO_SHA=b4388d8733fd49698848948bd9bac82a828374dc
WORKDIR /build
RUN git clone https://github.com/ka9q/ka9q-radio.git \
    && git -C ka9q-radio checkout "${KA9Q_RADIO_SHA}"

WORKDIR /build/ka9q-radio

# Apply local compatibility patches against the pinned SHA.
# See docker/ka9q-radio/patches/README.md and docs/ka9q-compat-audit.md.
# COPY the whole patches/ tree (not just *.patch) so the build succeeds
# even when zero active patches exist — disabled/historical patches kept
# in-tree under *.patch.disabled are intentionally skipped by the glob.
COPY docker/ka9q-radio/patches/ /tmp/patches/
RUN applied=0; \
    for p in /tmp/patches/*.patch; do \
        [ -e "$p" ] || break; \
        echo "Applying $p"; \
        git apply --whitespace=nowarn "$p"; \
        applied=$((applied+1)); \
    done; \
    echo "ka9q-radio patches applied: $applied"

# Build only what we need: radiod core + rx888.so plugin + control utility
# Disable all other SDR hardware to avoid pulling in unnecessary -dev packages
RUN make -C src -j$(nproc) \
    ENABLE_AIRSPY=0 \
    ENABLE_AIRSPYHF=0 \
    ENABLE_BLADERF=0 \
    ENABLE_FOBOS=0 \
    ENABLE_FUNCUBE=0 \
    ENABLE_HACKRF=0 \
    ENABLE_HYDRASDR=0 \
    ENABLE_RTLSDR=0 \
    ENABLE_RX888=1 \
    ENABLE_SDRPLAY=0 \
    ENABLE_SIG_GEN=1 \
    ARCHOPTS="" \
    radiod rx888.so sig_gen.so control tune monitor powers pcmrecord

# Build libonion (web framework ka9q-web links against) from source — not
# packaged in Debian. SSL/gnutls left at default (on) to match the reference
# deployment's binary; other optional backends disabled to avoid extra deps.
RUN git clone --depth 1 https://github.com/davidmoreno/onion.git /build/onion \
    && cmake -S /build/onion -B /build/onion/build \
         -DCMAKE_INSTALL_PREFIX=/usr/local \
         -DONION_USE_PAM=false -DONION_USE_PNG=false -DONION_USE_JPEG=false \
         -DONION_USE_XML2=false -DONION_USE_SYSTEMD=false -DONION_USE_SQLITE3=false \
         -DONION_USE_REDIS=false -DONION_USE_GC=false -DONION_USE_TESTS=false \
         -DONION_EXAMPLES=false -DONION_USE_BINDINGS_CPP=false \
    && cmake --build /build/onion/build -j"$(nproc)" \
    && cmake --install /build/onion/build \
    && ldconfig

# Build ka9q-web as a sibling of the ka9q-radio tree (its Makefile uses
# KA9Q_RADIO_DIR=../ka9q-radio/src and links that tree's multicast.o/
# status.o/misc.o/decode_status.o/rtp.o). Pinned to aba3ab4 (HEAD, 2026-06-15)
# to pair newest-with-newest against the ka9q-radio SHA above. ka9q-web is an
# independent moving part: this link step is the decisive compatibility check.
# If it fails to build, the firmware smoke test does NOT need ka9q-web — pin a
# ka9q-web commit known to pair with this ka9q-radio SHA, or drop this step.
ENV LIBRARY_PATH=/usr/local/lib C_INCLUDE_PATH=/usr/local/include
RUN git clone https://github.com/wa2n-code/ka9q-web.git /build/ka9q-web \
    && git -C /build/ka9q-web checkout aba3ab4af281f4dcb63c76e1b499dd3c40f7855a \
    && make -C /build/ka9q-web -j"$(nproc)"

# ---- Runtime image ----
FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    libavahi-client3 \
    libbsd0 \
    libfftw3-single3 \
    libiniparser1 \
    libncursesw6 \
    libopus0 \
    libusb-1.0-0 \
    libogg0 \
    libsamplerate0 \
    libportaudio2 \
    libfftw3-bin \
    avahi-daemon \
    avahi-utils \
    libnss-mdns \
    dbus \
    procps \
    usbutils \
    libgnutls30 \
    libgcrypt20 \
    python3 \
    python3-serial \
    sox \
    alsa-utils \
    && rm -rf /var/lib/apt/lists/*

# Copy built binaries
COPY --from=builder /build/ka9q-radio/src/radiod /usr/local/bin/
COPY --from=builder /build/ka9q-radio/src/control /usr/local/bin/
COPY --from=builder /build/ka9q-radio/src/tune /usr/local/bin/
COPY --from=builder /build/ka9q-radio/src/monitor /usr/local/bin/
COPY --from=builder /build/ka9q-radio/src/powers /usr/local/bin/
COPY --from=builder /build/ka9q-radio/src/pcmrecord /usr/local/bin/
COPY --from=builder /build/ka9q-radio/src/rx888.so /usr/local/lib/ka9q-radio/
COPY --from=builder /build/ka9q-radio/src/sig_gen.so /usr/local/lib/ka9q-radio/

# ka9q-web (web spectrum/waterfall viewer) + its libonion dependency.
# Run inside the container, e.g.:
#   docker exec -d ka9q-radio ka9q-web -m hf.local -p 8081 -n rx888-test
# then browse http://localhost:8081 (under --network host ka9q-web binds host :8081 directly).
COPY --from=builder /usr/local/lib/libonion*.so* /usr/local/lib/
COPY --from=builder /build/ka9q-web/ka9q-web /usr/local/sbin/ka9q-web
COPY --from=builder /build/ka9q-web/html /usr/local/share/ka9q-web/html
RUN ldconfig

# Create plugin directory and config directory
RUN mkdir -p /usr/local/lib/ka9q-radio /etc/radio /var/lib/ka9q-radio /usr/local/share/ka9q-radio

# Install ka9q-radio share files (presets.conf, bandplan.txt, help.txt, etc.)
# radiod loads /usr/local/share/ka9q-radio/presets.conf at startup.
COPY --from=builder /build/ka9q-radio/share/ /usr/local/share/ka9q-radio/

# Install minimal rx888 config for HF direct-sampling test
COPY docker/ka9q-radio/rx888-test.conf /etc/radio/radiod@rx888-test.conf

# Copy bench tools (QDX CAT, audio, RF test, rung scripts)
COPY tests/bench/qdx_cat.py         /usr/local/lib/bench/
COPY tests/bench/qdx_audio.py       /usr/local/lib/bench/
COPY tests/bench/bench_rf_test.py   /usr/local/lib/bench/
COPY tests/bench/rung2a_cat_test.py /usr/local/lib/bench/
COPY tests/bench/rung2b_audio_test.py /usr/local/lib/bench/
COPY tests/bench/rung3_loopback_test.py /usr/local/lib/bench/

# Copy entrypoint
COPY docker/ka9q-radio/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]
CMD ["radiod", "/etc/radio/radiod@rx888-test.conf"]
