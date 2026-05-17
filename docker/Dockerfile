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

# Default to a writable workdir; run --user maps host UID at runtime so
# bind-mounted outputs are owned by the host user.
WORKDIR /work

CMD ["/bin/bash"]
