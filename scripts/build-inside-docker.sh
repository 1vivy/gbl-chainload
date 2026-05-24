#!/usr/bin/env bash
# In-container build steps. Invoked by scripts/build.sh; not meant to be
# called directly from the host.
#
# Env vars consumed:
#   GBL_AUTO    — 0/1 (default 0)
#   GBL_DEBUG   — 0/1 (default 0)
#   GBL_VERBOSE — 0/1 (default 0)
#
# Engine rework (Task 11): GBL_MODE is gone. One EFI for every profile;
# activation is driven by the runtime GBLP1 manifest, not compile flags.
set -euo pipefail

BUILD_TARGET="${BUILD_TARGET:-RELEASE}"
TOOLCHAIN_TAG="${TOOLCHAIN_TAG:-CLANG35}"
ARCH="${ARCH:-AARCH64}"

GBL_AUTO="${GBL_AUTO:-0}"
GBL_DEBUG="${GBL_DEBUG:-0}"
GBL_VERBOSE="${GBL_VERBOSE:-0}"

if [[ -z "${GBL_CHAINLOAD_VERSION:-}" ]]; then
  echo "error: GBL_CHAINLOAD_VERSION not set (must come from host VERSION file)" >&2
  exit 1
fi

# Single-source-of-truth build identifier. Same shape as build.sh's artifact
# filename so dist/<NAME>.efi and the on-device getvar gbl-chainload_build
# match. The DSC substitutes this into FastbootCmds (getvar), FastbootMenu
# (display), and LogFsLib (banner).
if [[ -z "${GBL_BUILD_NAME:-}" ]]; then
  GBL_BUILD_NAME_SUFFIX=""
  if [[ $GBL_AUTO    -eq 1 ]]; then GBL_BUILD_NAME_SUFFIX+="-auto";    fi
  if [[ $GBL_DEBUG   -eq 1 ]]; then GBL_BUILD_NAME_SUFFIX+="-debug";   fi
  if [[ $GBL_VERBOSE -eq 1 ]]; then GBL_BUILD_NAME_SUFFIX+="-verbose"; fi
  GBL_BUILD_NAME="gbl-chainload${GBL_BUILD_NAME_SUFFIX}"
fi

# CLANG35 toolchain expects CLANG_BIN (clang directory) and CLANG_PREFIX
# (cross binutils prefix). Ubuntu's gcc-aarch64-linux-gnu provides
# /usr/bin/aarch64-linux-gnu-{ld,objcopy,strip,...}; clang itself is
# at /usr/bin/clang.
export CLANG_BIN="${CLANG_BIN:-/usr/bin/}"
export CLANG_PREFIX="${CLANG_PREFIX:-aarch64-linux-gnu-}"

cd /work

# EDK2 BaseTools build env. edksetup.sh expects to be sourced from edk2 root.
export WORKSPACE=/work
export PACKAGES_PATH="/work:/work/edk2"
export EDK_TOOLS_PATH="/work/edk2/BaseTools"
# Keep EDK2's generated tools_def.txt / build_rule.txt / target.txt out of
# the repo's conf/ dir. Place them under Build/, which is gitignored.
export CONF_PATH="/work/Build/Conf"
mkdir -p "$CONF_PATH"

# The first ever build (or a clean tree) needs BaseTools compiled.
if [[ ! -x edk2/BaseTools/Source/C/bin/GenFv ]]; then
  echo ">>> building EDK2 BaseTools (one-time)"
  make -C edk2/BaseTools -j"$(nproc)"
fi

# Source edksetup AFTER BaseTools exists.
set +u
pushd edk2 >/dev/null
. ./edksetup.sh BaseTools
popd >/dev/null
set -u

export GCC5_AARCH64_PREFIX=/usr/bin/aarch64-linux-gnu-

# PR2 Task 4: GblPayloadLib's parser / SHA-256 / CRC-32 now live in the
# crates/gblp1 Rust staticlib. EDK2's GCC link path (via aarch64-linux-
# gnu-ld) wants ELF objects, so we target `aarch64-unknown-none`
# (bare-metal ELF) — not `aarch64-unknown-uefi`, which emits COFF/PE
# and fails with "file format not recognized" at link time. The crate
# is fully no_std under target_os = "uefi" (which `unknown-none` is
# not), so we also need the host-style cfg to NOT activate the panic
# handler — but the panic handler is target_os = "uefi"-gated, which
# is exactly what `unknown-none` is NOT, so the crate compiles
# cleanly. The `--no-default-features` flag disables the
# `alloc`-gated `pack()` function (firmware never packs).
echo ">>> cargo build: crates/gblp1 (aarch64-unknown-none ELF staticlib)"
cargo build --release --target aarch64-unknown-none -p gblp1 --no-default-features

# PR2 Task 5: same pattern as crates/gblp1 — the firmware-side
# mode2_profile parser (formerly Mode2Profile.c) lives in the Rust
# `crates/mode2-profile-core` staticlib. `--no-default-features`
# strips the host-only `compile` + `derive` paths (and their `toml` /
# `serde` deps) so the firmware staticlib is the parser only.
echo ">>> cargo build: crates/mode2-profile-core (aarch64-unknown-none ELF staticlib)"
cargo build --release --target aarch64-unknown-none -p mode2-profile-core --no-default-features

# PR2 Task 6: the dynamic patch engine moved into crates/patch-engine.
# `--no-default-features` strips the host-only OEM + retired modules
# from the firmware staticlib — only the abl_permissive group
# (patch6 + patch10) ships on-device.
echo ">>> cargo build: crates/patch-engine (aarch64-unknown-none ELF staticlib)"
cargo build --release --target aarch64-unknown-none -p patch-engine --no-default-features

# PR2 Task 7: AVB structure parsing moved into crates/avb-parse.
# `--no-default-features` strips std + the host-only features so the
# staticlib is pure no_std parser. Used by FastbootCmds.c (chain-verdict
# probe, vbmeta lookup, hash descriptor walk) on the firmware side and
# by vbmeta-graft / mode2-profile / tests/avb on the host side.
echo ">>> cargo build: crates/avb-parse (aarch64-unknown-none ELF staticlib)"
cargo build --release --target aarch64-unknown-none -p avb-parse --no-default-features

echo ">>> build: $TOOLCHAIN_TAG / $ARCH / $BUILD_TARGET / name=$GBL_BUILD_NAME auto=$GBL_AUTO debug=$GBL_DEBUG verbose=$GBL_VERBOSE"
build \
  -p GblChainloadPkg/GblChainloadPkg.dsc \
  -a "$ARCH" \
  -t "$TOOLCHAIN_TAG" \
  -b "$BUILD_TARGET" \
  -D GBL_AUTO="$GBL_AUTO" \
  -D GBL_DEBUG="$GBL_DEBUG" \
  -D GBL_VERBOSE="$GBL_VERBOSE" \
  -D GBL_BUILD_NAME="$GBL_BUILD_NAME" \
  -D GBL_CHAINLOAD_VERSION="$GBL_CHAINLOAD_VERSION"

# Verify expected output exists.
EFI_OUT="Build/GblChainloadPkg/${BUILD_TARGET}_${TOOLCHAIN_TAG}/${ARCH}/GblChainload.efi"
if [[ ! -f "$EFI_OUT" ]]; then
  echo "error: expected output not found at $EFI_OUT" >&2
  echo "       searching for any GblChainload.efi:" >&2
  find Build -name 'GblChainload.efi' -print 2>&1 | head -5 >&2 || true
  exit 1
fi

mkdir -p dist
cp "$EFI_OUT" dist/gbl-chainload.efi
echo ">>> done: dist/gbl-chainload.efi"
ls -l dist/gbl-chainload.efi
