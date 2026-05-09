#!/usr/bin/env bash
# In-container build steps. Invoked by scripts/build.sh; not meant to be
# called directly from the host.
#
# Env vars consumed:
#   GBL_MODE    — integer 1/2/3 (default 1)
#   GBL_AUTO    — 0/1 (default 0)
#   GBL_DEBUG   — 0/1 (default 0)
#   GBL_VERBOSE — 0/1 (default 0)
set -euo pipefail

BUILD_TARGET="${BUILD_TARGET:-RELEASE}"
TOOLCHAIN_TAG="${TOOLCHAIN_TAG:-CLANG35}"
ARCH="${ARCH:-AARCH64}"

GBL_MODE="${GBL_MODE:-1}"
GBL_AUTO="${GBL_AUTO:-0}"
GBL_DEBUG="${GBL_DEBUG:-0}"
GBL_VERBOSE="${GBL_VERBOSE:-0}"

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

echo ">>> build: $TOOLCHAIN_TAG / $ARCH / $BUILD_TARGET / mode=$GBL_MODE auto=$GBL_AUTO debug=$GBL_DEBUG verbose=$GBL_VERBOSE"
build \
  -p GblChainloadPkg/GblChainloadPkg.dsc \
  -a "$ARCH" \
  -t "$TOOLCHAIN_TAG" \
  -b "$BUILD_TARGET" \
  -D GBL_MODE="$GBL_MODE" \
  -D GBL_AUTO="$GBL_AUTO" \
  -D GBL_DEBUG="$GBL_DEBUG" \
  -D GBL_VERBOSE="$GBL_VERBOSE"

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
