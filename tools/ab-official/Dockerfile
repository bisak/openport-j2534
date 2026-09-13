# Runs the vendor Windows J2534 DLL on an arm64 host: box64 translates the
# x86-64 Wine, Wine's new WoW64 layer runs the 32-bit DLL. No Windows, no VM.
#
#   docker build -t openport-ab-official tools/ab-official
#
# Why this particular stack (all measured, 2026-09-13, Apple M4 Max):
#   - Wine for macOS (WoW64):        the DLL's protector raises exceptions on
#                                    purpose; Wine's macOS WoW64 exception path
#                                    faults inside itself and the DLL never loads.
#   - Docker amd64 + Rosetta + Wine: Rosetta aborts on Wine's signal-return
#                                    (xsave state assertion).
#   - Docker i386 + qemu-user + Wine: Wine's mmap-alignment assertion under qemu.
#   - box64 + Wine wow64 (this):     works, with BOX64_DYNAREC=0; the dynarec
#                                    mis-executes the protector's self-modifying
#                                    code. Interpreter speed is ample: the DLL is
#                                    I/O bound.
FROM debian:bookworm-slim
ENV DEBIAN_FRONTEND=noninteractive
ARG WINE_VERSION=11.17
ARG WINE_SHA256=3211db09086bc6fbd769c0286f27790b32d6fc6fb0bc610dad6b59796f07d462
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates wget gnupg xz-utils python3 socat procps \
      libfreetype6 libgnutls30 libgcc-s1 libstdc++6 libunwind8 libx11-6 libxext6 \
      libfontconfig1 libkrb5-3 libgssapi-krb5-2 libxml2 libxslt1.1 libusb-1.0-0 libdbus-1-3 \
 && wget -qO /etc/apt/sources.list.d/box64.list https://ryanfortner.github.io/box64-debs/box64.list \
 && wget -qO- https://ryanfortner.github.io/box64-debs/KEY.gpg | gpg --dearmor -o /etc/apt/trusted.gpg.d/box64-debs-archive-keyring.gpg \
 && apt-get update && apt-get install -y --no-install-recommends box64-m1 \
 && rm -rf /var/lib/apt/lists/*
RUN wget -qO /tmp/wine.tar.xz "https://github.com/Kron4ek/Wine-Builds/releases/download/${WINE_VERSION}/wine-${WINE_VERSION}-amd64-wow64.tar.xz" \
 && echo "${WINE_SHA256}  /tmp/wine.tar.xz" | sha256sum -c - \
 && mkdir -p /opt/wine && tar -xJf /tmp/wine.tar.xz -C /opt/wine --strip-components=1 && rm /tmp/wine.tar.xz
ENV PATH=/opt/wine/bin:$PATH WINEPREFIX=/wine WINEDEBUG=-all WINEDLLOVERRIDES=winemenubuilder.exe=d BOX64_DYNAREC=0 BOX64_LOG=0
COPY iface.reg /opt/ab/iface.reg
# Boot the prefix and register the fake OpenPort device interface so that the
# DLL's SetupDi enumeration finds one device; its symbolic link resolves through
# dosdevices/ to whatever pty container-run.sh links there.
RUN box64 wineboot -u > /dev/null 2>&1 && box64 wineserver -w \
 && box64 wine regedit /S /opt/ab/iface.reg > /dev/null 2>&1 && box64 wineserver -w \
 && box64 wineserver -k || true
WORKDIR /out
ENTRYPOINT ["/repo/tools/ab-official/container-run.sh"]
