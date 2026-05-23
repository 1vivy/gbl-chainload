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
#   mingw-w64                                                    → x86_64-w64-mingw32-dlltool for windows-sys
#                                                                  (Rust 1.85+ x86_64-pc-windows-gnu cross-build;
#                                                                  zig cc alone is not enough)
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
        mingw-w64 \
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

# --- zig toolchain for Windows/macOS cross-compilation ---
# zig cc cross-compiles the host-side tools (legacy C and the future Rust
# tools/gbl multicall) to x86_64-windows-gnu and x86_64/aarch64-macos from
# this Linux image: it bundles the mingw-w64 and macOS SDK headers, so
# there is no license-restricted SDK to provision. The Android path stays
# on the NDK above, untouched.
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

# --- Rust toolchain ---
# Stable Rust + seven cross targets for the workspace at the repo root
# (crates/* core libraries + tools/gbl multicall binary). Installed
# system-wide under /opt/{rustup,cargo} so `--user` runtime invocations
# still find cargo. The aarch64-unknown-none target produces the ELF
# staticlibs the EDK2 firmware GCC link path consumes (cargo's
# aarch64-unknown-uefi target emits COFF/PE objects, which EDK2's GNU
# ld backend cannot read — see GblChainloadPkg.dsc DLINK2_FLAGS for
# the link-site detail). aarch64-unknown-uefi remains pre-installed
# for Rust EFI applications and unit tests that link the host stack.
# The five host cross targets cover the same release matrix as
# build-cross-tools.sh.
ARG RUST_VER=1.85.0
ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo
ENV PATH=/opt/cargo/bin:$PATH
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
      | sh -s -- -y --profile minimal --default-toolchain "${RUST_VER}" \
            -c clippy -c rustfmt \
 && /opt/cargo/bin/rustup target add \
        aarch64-unknown-uefi \
        aarch64-unknown-none \
        aarch64-linux-android \
        x86_64-pc-windows-gnu \
        x86_64-apple-darwin \
        aarch64-apple-darwin \
        x86_64-unknown-linux-musl \
 && chmod -R a+rwX /opt/rustup /opt/cargo
ENV NDK_BIN=/opt/android-ndk/toolchains/llvm/prebuilt/linux-x86_64/bin
ENV CC_aarch64_linux_android=$NDK_BIN/aarch64-linux-android31-clang \
    CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER=$NDK_BIN/aarch64-linux-android31-clang

# Tiny zig-cc wrapper scripts (each pins the target triple for cargo's
# linker — see /.cargo/config.toml). zig cc accepts -target as a first-
# class flag; cargo just needs an executable that knows its target.
RUN for spec in \
        "windows-x64:x86_64-windows-gnu" \
        "macos-x64:x86_64-macos" \
        "macos-arm64:aarch64-macos" \
        "linux-musl:x86_64-linux-musl"; do \
      name=${spec%%:*} ; tgt=${spec#*:}; \
      printf '#!/bin/sh\nexec /opt/zig/zig cc -target %s "$@"\n' "$tgt" \
        > /usr/local/bin/zig-cc-${name} && chmod +x /usr/local/bin/zig-cc-${name}; \
    done

# Default to a writable workdir; run --user maps host UID at runtime so
# bind-mounted outputs are owned by the host user.
WORKDIR /work

CMD ["/bin/bash"]
