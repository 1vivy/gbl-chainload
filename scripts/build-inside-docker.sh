#!/usr/bin/env bash
# In-container build steps. Invoked by scripts/build.sh; not meant to be
# called directly from the host.
#
# Env vars consumed:
#   GBL_MODE    — integer 0/1 today (default 1); mode 2 is planned
#   GBL_AUTO    — 0/1 (default 0)
#   GBL_DEBUG   — 0/1 (default 0)
#   GBL_VERBOSE — 0/1 (default 0)
#   GBL_HAS_CACHED_ABL — 0/1 (default 0)
#   GBL_CACHE_ABL_INPUT — /work-visible raw ABL/FV image when cached ABL enabled
set -euo pipefail

BUILD_TARGET="${BUILD_TARGET:-RELEASE}"
TOOLCHAIN_TAG="${TOOLCHAIN_TAG:-CLANG35}"
ARCH="${ARCH:-AARCH64}"

GBL_MODE="${GBL_MODE:-1}"
GBL_AUTO="${GBL_AUTO:-0}"
GBL_DEBUG="${GBL_DEBUG:-0}"
GBL_VERBOSE="${GBL_VERBOSE:-0}"
GBL_HAS_CACHED_ABL="${GBL_HAS_CACHED_ABL:-0}"
GBL_CACHE_ABL_INPUT="${GBL_CACHE_ABL_INPUT:-}"

# Single-source-of-truth build identifier. Same shape as build.sh's artifact
# filename so dist/<NAME>.efi and the on-device getvar build-name match. The
# DSC substitutes this into FastbootCmds (getvar), FastbootMenu (display),
# and LogFsLib (banner).
if [[ -z "${GBL_BUILD_NAME:-}" ]]; then
  GBL_BUILD_NAME_SUFFIX=""
  [[ $GBL_AUTO    -eq 1 ]] && GBL_BUILD_NAME_SUFFIX+="-auto"
  [[ $GBL_DEBUG   -eq 1 ]] && GBL_BUILD_NAME_SUFFIX+="-debug"
  [[ $GBL_VERBOSE -eq 1 ]] && GBL_BUILD_NAME_SUFFIX+="-verbose"
  GBL_BUILD_NAME="mode-${GBL_MODE}${GBL_BUILD_NAME_SUFFIX}"
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

BUILD_ARGS=(
  -p GblChainloadPkg/GblChainloadPkg.dsc \
  -a "$ARCH" \
  -t "$TOOLCHAIN_TAG" \
  -b "$BUILD_TARGET" \
  -D GBL_MODE="$GBL_MODE" \
  -D GBL_AUTO="$GBL_AUTO" \
  -D GBL_DEBUG="$GBL_DEBUG" \
  -D GBL_VERBOSE="$GBL_VERBOSE" \
  -D GBL_HAS_CACHED_ABL="$GBL_HAS_CACHED_ABL" \
  -D GBL_BUILD_NAME="$GBL_BUILD_NAME"
)

if [[ "$GBL_HAS_CACHED_ABL" -eq 1 ]]; then
  if [[ -z "$GBL_CACHE_ABL_INPUT" || ! -f "$GBL_CACHE_ABL_INPUT" ]]; then
    echo "error: GBL_CACHE_ABL_INPUT missing or unreadable: $GBL_CACHE_ABL_INPUT" >&2
    exit 2
  fi
  echo ">>> cache-abl: preparing cached ABL from $GBL_CACHE_ABL_INPUT"
  make -C tools/fv-unwrap
  make -C tools/abl-patcher GBL_MODE="$GBL_MODE"
  mkdir -p Build/cache-abl
  tools/fv-unwrap/fv-unwrap "$GBL_CACHE_ABL_INPUT" Build/cache-abl/extracted-abl.efi
  tools/abl-patcher/abl-patcher --in Build/cache-abl/extracted-abl.efi --out Build/cache-abl/patched-abl.efi
  scripts/generate-cached-abl-header.py \
    --source "$GBL_CACHE_ABL_INPUT" \
    --extracted-pe Build/cache-abl/extracted-abl.efi \
    --patched-pe Build/cache-abl/patched-abl.efi \
    --mode "$GBL_MODE" \
    --out GblChainloadPkg/Library/CachedAblLib/CachedAblBlob.generated.h
fi

echo ">>> build: $TOOLCHAIN_TAG / $ARCH / $BUILD_TARGET / mode=$GBL_MODE auto=$GBL_AUTO debug=$GBL_DEBUG verbose=$GBL_VERBOSE cache_abl=$GBL_HAS_CACHED_ABL"
build "${BUILD_ARGS[@]}"

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
