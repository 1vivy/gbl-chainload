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

# Default to a writable workdir; run --user maps host UID at runtime so
# bind-mounted outputs are owned by the host user.
WORKDIR /work

CMD ["/bin/bash"]
