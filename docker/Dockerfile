# Build environment for gbl-chainload (and reproducible enough to build
# the legacy gbl_root_canoe on a Mac/Windows host via container).
#
# Used both for local dev and GitHub Actions CI.
#
# Usage:
#   docker build -t gbl-chainload-build:latest -f docker/Dockerfile .
#   docker run --rm -v "$PWD:/work" -w /work \
#       --user "$(id -u):$(id -g)" \
#       gbl-chainload-build:latest \
#       ./scripts/build.sh canoe

FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive

# Toolchain + EDK2 build deps.
#
# Why each:
#   build-essential, python3, make, git, ca-certificates, curl  → EDK2 BaseTools + submodule fetches
#   nasm                                                         → EDK2 BaseTools assembler
#   iasl                                                         → ACPI compiler (some Pkgs)
#   uuid-dev                                                     → libuuid headers (GenFv etc.)
#   clang, lld, llvm                                             → CLANGPDB / CLANGDWARF toolchains
#   gcc-aarch64-linux-gnu, binutils-aarch64-linux-gnu            → CLANG35 (legacy gbl_root_canoe path)
#   xxd, zip, unzip, file                                        → packaging / smoke tests
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        python3 \
        python3-distutils \
        python-is-python3 \
        make \
        git \
        nasm \
        iasl \
        uuid-dev \
        clang \
        lld \
        llvm \
        gcc-aarch64-linux-gnu \
        g++-aarch64-linux-gnu \
        binutils-aarch64-linux-gnu \
        ca-certificates \
        curl \
        xxd \
        zip \
        unzip \
        file \
    && rm -rf /var/lib/apt/lists/*

# --- Android NDK r27 for cross-compiling recovery tools ---
ARG NDK_VER=r27c
RUN curl -fsSL -o /tmp/ndk.zip \
        "https://dl.google.com/android/repository/android-ndk-${NDK_VER}-linux.zip" \
 && unzip -q /tmp/ndk.zip -d /opt \
 && mv /opt/android-ndk-${NDK_VER} /opt/android-ndk \
 && rm /tmp/ndk.zip
ENV ANDROID_NDK=/opt/android-ndk

# --- Static liblzma for aarch64-Android (cross-compiled xz-utils) ---
# fv-unwrap needs LZMA decode in its aarch64 static binary; the NDK
# sysroot has no liblzma. Cross-compile xz-utils once here so the
# Android build links a real -llzma (same as the host build) instead
# of vendoring the LZMA SDK source. 5.6.3 is post-CVE-2024-3094 (that
# backdoor lived in 5.6.0/5.6.1 release tarballs; the decoder was never
# affected). API level 31 matches aarch64-linux-android31-clang.
ARG XZ_VER=5.6.3
RUN set -eux; \
    TOOLBIN=/opt/android-ndk/toolchains/llvm/prebuilt/linux-x86_64/bin; \
    API=31; \
    curl -fsSL -o /tmp/xz.tar.gz \
      "https://github.com/tukaani-project/xz/releases/download/v${XZ_VER}/xz-${XZ_VER}.tar.gz"; \
    mkdir -p /tmp/xz && tar -xf /tmp/xz.tar.gz -C /tmp/xz --strip-components=1; \
    cd /tmp/xz; \
    ./configure \
      --host=aarch64-linux-android \
      --prefix=/opt/liblzma-android \
      --enable-static --disable-shared \
      --disable-xz --disable-xzdec --disable-lzmadec --disable-lzmainfo \
      --disable-lzma-links --disable-scripts --disable-doc \
      --disable-nls --disable-threads \
      CC="${TOOLBIN}/aarch64-linux-android${API}-clang" \
      AR="${TOOLBIN}/llvm-ar" \
      RANLIB="${TOOLBIN}/llvm-ranlib" \
      STRIP="${TOOLBIN}/llvm-strip" \
      CFLAGS="-O2 -fPIC"; \
    make -j"$(nproc)" && make install; \
    rm -rf /tmp/xz /tmp/xz.tar.gz
ENV LIBLZMA_ANDROID=/opt/liblzma-android

# --- zig toolchain for Windows/macOS cross-compilation ---
# zig cc cross-compiles the host-side tools to x86_64-windows-gnu and
# x86_64/aarch64-macos from this Linux image: it bundles the mingw-w64
# and macOS SDK headers, so there is no license-restricted SDK to
# provision. The Android path stays on the NDK above, untouched.
ARG ZIG_VER=0.13.0
RUN curl -fsSL -o /tmp/zig.tar.xz \
        "https://ziglang.org/download/${ZIG_VER}/zig-linux-x86_64-${ZIG_VER}.tar.xz" \
 && mkdir -p /opt/zig \
 && tar -xJf /tmp/zig.tar.xz -C /opt/zig --strip-components=1 \
 && rm /tmp/zig.tar.xz
ENV PATH=/opt/zig:$PATH

# llvm-lipo (from the already-installed `llvm` apt package, version-
# suffixed under /usr/lib/llvm-*/bin) onto PATH for the macOS
# universal-binary merge step in build-cross-tools.sh.
RUN ln -sf "$(ls /usr/lib/llvm-*/bin/llvm-lipo | head -1)" /usr/local/bin/llvm-lipo

# --- Static liblzma for the Windows/macOS cross targets ---
# fv-unwrap needs LZMA decode; zig's bundled target sysroots have no
# liblzma. Cross-build xz-utils once per target with zig cc as CC
# (llvm-ar/llvm-ranlib are object-format-agnostic). Same xz $(XZ_VER)
# as the Android block above.
RUN set -eux; \
    curl -fsSL -o /tmp/xz.tar.gz \
      "https://github.com/tukaani-project/xz/releases/download/v${XZ_VER}/xz-${XZ_VER}.tar.gz"; \
    \
    # Stub RC "compiler" for the Windows cross target: xz-utils generates a
    # liblzma_w32res.rc DLL-version-info resource; we only build the static
    # library, so the resource object is unused. However autotools/libtool
    # still invokes RC and expects an output .obj.  This stub extracts the
    # -o <outfile> argument and writes a minimal valid Windows COFF object
    # (an empty translation-unit compiled with zig cc) so llvm-ar is happy.
    printf '%s\n' '#!/bin/sh' \
      'outfile=""' \
      'while [ $# -gt 0 ]; do' \
      '  case "$1" in -o) outfile="$2"; shift 2;; -o=*) outfile="${1#-o=}"; shift;; -o*) outfile="${1#-o}"; shift;; *) shift;; esac' \
      'done' \
      '[ -n "$outfile" ] || { echo "zig-rc-win: no -o <outfile> found in args" >&2; exit 1; }' \
      'printf "typedef void empty;\n" > /tmp/_rc_empty.c' \
      'zig cc -target x86_64-windows-gnu -c /tmp/_rc_empty.c -o "$outfile"' \
      > /usr/local/bin/zig-rc-win && chmod +x /usr/local/bin/zig-rc-win; \
    \
    for spec in \
        "x86_64-windows-gnu:x86_64-w64-mingw32:/opt/liblzma-windows" \
        "x86_64-macos:x86_64-apple-darwin:/opt/liblzma-macos-x64" \
        "aarch64-macos:aarch64-apple-darwin:/opt/liblzma-macos-arm64" \
        "x86_64-linux-musl:x86_64-linux-musl:/opt/liblzma-linux" ; do \
      ztgt="${spec%%:*}"; rest="${spec#*:}"; \
      host="${rest%%:*}"; prefix="${rest#*:}"; \
      rc_arg=true; \
      [ "$ztgt" = "x86_64-windows-gnu" ] && rc_arg=/usr/local/bin/zig-rc-win; \
      rm -rf /tmp/xz && mkdir -p /tmp/xz; \
      tar -xf /tmp/xz.tar.gz -C /tmp/xz --strip-components=1; \
      cd /tmp/xz; \
      ./configure \
        --host="$host" \
        --prefix="$prefix" \
        --enable-static --disable-shared \
        --disable-xz --disable-xzdec --disable-lzmadec --disable-lzmainfo \
        --disable-lzma-links --disable-scripts --disable-doc \
        --disable-nls --disable-threads \
        CC="zig cc -target $ztgt" \
        AR=llvm-ar RANLIB=llvm-ranlib \
        RC="$rc_arg" \
        CFLAGS="-O2"; \
      make -j"$(nproc)"; make install; \
      cd /; \
    done; \
    rm -rf /tmp/xz /tmp/xz.tar.gz
ENV LIBLZMA_WINDOWS=/opt/liblzma-windows
ENV LIBLZMA_MACOS_X64=/opt/liblzma-macos-x64
ENV LIBLZMA_MACOS_ARM64=/opt/liblzma-macos-arm64
ENV LIBLZMA_LINUX=/opt/liblzma-linux

# Default to a writable workdir; run --user maps host UID at runtime so
# bind-mounted outputs are owned by the host user.
WORKDIR /work

CMD ["/bin/bash"]
