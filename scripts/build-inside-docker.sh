#!/usr/bin/env bash
# In-container build steps. Invoked by scripts/build.sh; not meant to be
# called directly from the host.
set -euo pipefail

BUILD_TARGET="${BUILD_TARGET:-RELEASE}"
TOOLCHAIN_TAG="${TOOLCHAIN_TAG:-CLANG35}"
ARCH="${ARCH:-AARCH64}"
GBL_BUILD_MODE="${GBL_BUILD_MODE:-mode-debug}"
GBL_MODE_ARTIFACT="${GBL_MODE_ARTIFACT:-mode-debug.efi}"
GBL_DEBUG_VARIANT="${GBL_DEBUG_VARIANT:-full}"

MODE_DEFINES=()
case "$GBL_BUILD_MODE" in
  auto-debug)
    MODE_DEFINES=(-D GBL_AUTO_DEBUG_MODE=1 -D GBL_MODE_DEBUG=0 -D GBL_MINIMAL=0 -D GBL_MODE_TEMPLATE=0 -D GBL_FAKELOCKED=0 -D GBL_FAKELOCKED_DEBUG=0 -D GBL_MODE_1=0)
    ;;
  mode-debug)
    MODE_DEFINES=(-D GBL_AUTO_DEBUG_MODE=0 -D GBL_MODE_DEBUG=1 -D GBL_MINIMAL=0 -D GBL_MODE_TEMPLATE=0 -D GBL_FAKELOCKED=0 -D GBL_FAKELOCKED_DEBUG=0 -D GBL_MODE_1=0)
    ;;
  minimal)
    MODE_DEFINES=(-D GBL_AUTO_DEBUG_MODE=0 -D GBL_MODE_DEBUG=0 -D GBL_MINIMAL=1 -D GBL_MODE_TEMPLATE=0 -D GBL_FAKELOCKED=0 -D GBL_FAKELOCKED_DEBUG=0 -D GBL_MODE_1=0)
    ;;
  mode-template)
    MODE_DEFINES=(-D GBL_AUTO_DEBUG_MODE=0 -D GBL_MODE_DEBUG=0 -D GBL_MINIMAL=0 -D GBL_MODE_TEMPLATE=1 -D GBL_FAKELOCKED=0 -D GBL_FAKELOCKED_DEBUG=0 -D GBL_MODE_1=0)
    ;;
  mode-fakelocked|fakelocked)
    MODE_DEFINES=(-D GBL_AUTO_DEBUG_MODE=0 -D GBL_MODE_DEBUG=0 -D GBL_MINIMAL=0 -D GBL_MODE_TEMPLATE=0 -D GBL_FAKELOCKED=1 -D GBL_FAKELOCKED_DEBUG=0 -D GBL_MODE_1=0)
    ;;
  mode-fakelocked-debug|fakelocked-debug)
    MODE_DEFINES=(-D GBL_AUTO_DEBUG_MODE=0 -D GBL_MODE_DEBUG=0 -D GBL_MINIMAL=0 -D GBL_MODE_TEMPLATE=0 -D GBL_FAKELOCKED=0 -D GBL_FAKELOCKED_DEBUG=1 -D GBL_MODE_1=0)
    ;;
  mode-1)
    MODE_DEFINES=(-D GBL_AUTO_DEBUG_MODE=0 -D GBL_MODE_DEBUG=0 -D GBL_MINIMAL=0 -D GBL_MODE_TEMPLATE=0 -D GBL_FAKELOCKED=0 -D GBL_FAKELOCKED_DEBUG=0 -D GBL_MODE_1=1)
    ;;
  *)
    echo "error: unknown GBL_BUILD_MODE=$GBL_BUILD_MODE" >&2
    exit 1
    ;;
esac

DEBUG_DEFINES=(-D GBL_DEBUG_PHASE_FLUSH=0 -D GBL_DEBUG_PATCH_ONLY=0 \
  -D GBL_DEBUG_NO_EBS=0 -D GBL_DEBUG_EBS_SCAN=0 -D GBL_DEBUG_EBS_WRAPPER_ONLY=0 \
  -D GBL_DEBUG_EBS_FDT_PROBE=0 -D GBL_DEBUG_EBS_NO_BOOTCONFIG=0 -D GBL_DEBUG_EBS_NO_CLOSE=0 \
  -D GBL_DEBUG_EBS_ONLY=0 -D GBL_DEBUG_UDT_HELPER=0 -D GBL_DEBUG_UDT_HELPER_CMDLINE_ONLY=0)
case "$GBL_DEBUG_VARIANT" in
  full)
    ;;
  patch-only)
    DEBUG_DEFINES=(-D GBL_DEBUG_PHASE_FLUSH=1 -D GBL_DEBUG_PATCH_ONLY=1 \
      -D GBL_DEBUG_NO_EBS=0 -D GBL_DEBUG_EBS_SCAN=0 -D GBL_DEBUG_EBS_WRAPPER_ONLY=0 \
      -D GBL_DEBUG_EBS_FDT_PROBE=0 -D GBL_DEBUG_EBS_NO_BOOTCONFIG=0 -D GBL_DEBUG_EBS_NO_CLOSE=0)
    ;;
  no-ebs)
    DEBUG_DEFINES=(-D GBL_DEBUG_PHASE_FLUSH=1 -D GBL_DEBUG_PATCH_ONLY=0 \
      -D GBL_DEBUG_NO_EBS=1 -D GBL_DEBUG_EBS_SCAN=0 -D GBL_DEBUG_EBS_WRAPPER_ONLY=0 \
      -D GBL_DEBUG_EBS_FDT_PROBE=0 -D GBL_DEBUG_EBS_NO_BOOTCONFIG=0 -D GBL_DEBUG_EBS_NO_CLOSE=0)
    ;;
  ebs-wrapper-only)
    DEBUG_DEFINES=(-D GBL_DEBUG_PHASE_FLUSH=1 -D GBL_DEBUG_PATCH_ONLY=0 \
      -D GBL_DEBUG_NO_EBS=0 -D GBL_DEBUG_EBS_SCAN=0 -D GBL_DEBUG_EBS_WRAPPER_ONLY=1 \
      -D GBL_DEBUG_EBS_FDT_PROBE=0 -D GBL_DEBUG_EBS_NO_BOOTCONFIG=0 -D GBL_DEBUG_EBS_NO_CLOSE=0)
    ;;
  ebs-fdt-probe)
    DEBUG_DEFINES=(-D GBL_DEBUG_PHASE_FLUSH=1 -D GBL_DEBUG_PATCH_ONLY=0 \
      -D GBL_DEBUG_NO_EBS=0 -D GBL_DEBUG_EBS_SCAN=0 -D GBL_DEBUG_EBS_WRAPPER_ONLY=0 \
      -D GBL_DEBUG_EBS_FDT_PROBE=1 -D GBL_DEBUG_EBS_NO_BOOTCONFIG=0 -D GBL_DEBUG_EBS_NO_CLOSE=0 \
      -D GBL_DEBUG_EBS_ONLY=0)
    ;;
  ebs-fdt-probe-only)
    DEBUG_DEFINES=(-D GBL_DEBUG_PHASE_FLUSH=1 -D GBL_DEBUG_PATCH_ONLY=0 \
      -D GBL_DEBUG_NO_EBS=0 -D GBL_DEBUG_EBS_SCAN=0 -D GBL_DEBUG_EBS_WRAPPER_ONLY=0 \
      -D GBL_DEBUG_EBS_FDT_PROBE=1 -D GBL_DEBUG_EBS_NO_BOOTCONFIG=0 -D GBL_DEBUG_EBS_NO_CLOSE=0 \
      -D GBL_DEBUG_EBS_ONLY=1)
    ;;
  ebs-scan)
    DEBUG_DEFINES=(-D GBL_DEBUG_PHASE_FLUSH=1 -D GBL_DEBUG_PATCH_ONLY=0 \
      -D GBL_DEBUG_NO_EBS=0 -D GBL_DEBUG_EBS_SCAN=1 -D GBL_DEBUG_EBS_WRAPPER_ONLY=0 \
      -D GBL_DEBUG_EBS_FDT_PROBE=0 -D GBL_DEBUG_EBS_NO_BOOTCONFIG=0 -D GBL_DEBUG_EBS_NO_CLOSE=0)
    ;;
  ebs-no-bootconfig)
    DEBUG_DEFINES=(-D GBL_DEBUG_PHASE_FLUSH=1 -D GBL_DEBUG_PATCH_ONLY=0 \
      -D GBL_DEBUG_NO_EBS=0 -D GBL_DEBUG_EBS_SCAN=1 -D GBL_DEBUG_EBS_WRAPPER_ONLY=0 \
      -D GBL_DEBUG_EBS_FDT_PROBE=0 -D GBL_DEBUG_EBS_NO_BOOTCONFIG=1 -D GBL_DEBUG_EBS_NO_CLOSE=0)
    ;;
  ebs-no-close)
    DEBUG_DEFINES=(-D GBL_DEBUG_PHASE_FLUSH=1 -D GBL_DEBUG_PATCH_ONLY=0 \
      -D GBL_DEBUG_NO_EBS=0 -D GBL_DEBUG_EBS_SCAN=1 -D GBL_DEBUG_EBS_WRAPPER_ONLY=0 \
      -D GBL_DEBUG_EBS_FDT_PROBE=0 -D GBL_DEBUG_EBS_NO_BOOTCONFIG=0 -D GBL_DEBUG_EBS_NO_CLOSE=1)
    ;;
  udt-helper)
    DEBUG_DEFINES=(-D GBL_DEBUG_PHASE_FLUSH=1 -D GBL_DEBUG_PATCH_ONLY=0 \
      -D GBL_DEBUG_NO_EBS=0 -D GBL_DEBUG_EBS_SCAN=0 -D GBL_DEBUG_EBS_WRAPPER_ONLY=1 \
      -D GBL_DEBUG_EBS_FDT_PROBE=0 -D GBL_DEBUG_EBS_NO_BOOTCONFIG=0 -D GBL_DEBUG_EBS_NO_CLOSE=0 \
      -D GBL_DEBUG_EBS_ONLY=0 -D GBL_DEBUG_UDT_HELPER=1 -D GBL_DEBUG_UDT_HELPER_CMDLINE_ONLY=0)
    ;;
  udt-helper-cmdline-only)
    DEBUG_DEFINES=(-D GBL_DEBUG_PHASE_FLUSH=1 -D GBL_DEBUG_PATCH_ONLY=0 \
      -D GBL_DEBUG_NO_EBS=0 -D GBL_DEBUG_EBS_SCAN=0 -D GBL_DEBUG_EBS_WRAPPER_ONLY=1 \
      -D GBL_DEBUG_EBS_FDT_PROBE=0 -D GBL_DEBUG_EBS_NO_BOOTCONFIG=0 -D GBL_DEBUG_EBS_NO_CLOSE=0 \
      -D GBL_DEBUG_EBS_ONLY=0 -D GBL_DEBUG_UDT_HELPER=1 -D GBL_DEBUG_UDT_HELPER_CMDLINE_ONLY=1)
    ;;
  ebs-mutate)
    DEBUG_DEFINES=(-D GBL_DEBUG_PHASE_FLUSH=1 -D GBL_DEBUG_PATCH_ONLY=0 \
      -D GBL_DEBUG_NO_EBS=0 -D GBL_DEBUG_EBS_SCAN=0 -D GBL_DEBUG_EBS_WRAPPER_ONLY=0 \
      -D GBL_DEBUG_EBS_FDT_PROBE=0 -D GBL_DEBUG_EBS_NO_BOOTCONFIG=0 -D GBL_DEBUG_EBS_NO_CLOSE=0 \
      -D GBL_DEBUG_EBS_ONLY=0 -D GBL_DEBUG_UDT_HELPER=0 -D GBL_DEBUG_UDT_HELPER_CMDLINE_ONLY=0 \
      -D GBL_DEBUG_EBS_MUTATE=1)
    ;;
  *)
    echo "error: unknown GBL_DEBUG_VARIANT=$GBL_DEBUG_VARIANT" >&2
    exit 1
    ;;
esac

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
# the repo's conf/ dir (which is reserved for OUR template files). Place
# them under Build/, which is gitignored.
export CONF_PATH="/work/Build/Conf"
mkdir -p "$CONF_PATH"

# The first ever build (or a clean tree) needs BaseTools compiled.
# Patch c1 ("ignore basetool warnings", from abl-build-ci 0002) bakes
# -Wno-error into header.makefile, so no EXTRA_OPTFLAGS hack needed here.
if [[ ! -x edk2/BaseTools/Source/C/bin/GenFv ]]; then
  echo ">>> building EDK2 BaseTools (one-time)"
  make -C edk2/BaseTools -j"$(nproc)"
fi

# Source edksetup AFTER BaseTools exists. edksetup/BuildEnv references
# several env vars without initializing them, so relax `set -u` for the
# source. Re-enable after.
set +u
pushd edk2 >/dev/null
. ./edksetup.sh BaseTools
popd >/dev/null
set -u

export GCC5_AARCH64_PREFIX=/usr/bin/aarch64-linux-gnu-

echo ">>> build: $TOOLCHAIN_TAG / $ARCH / $BUILD_TARGET / mode=$GBL_BUILD_MODE / debug=$GBL_DEBUG_VARIANT"
build \
  -p GblChainloadPkg/GblChainloadPkg.dsc \
  -a "$ARCH" \
  -t "$TOOLCHAIN_TAG" \
  -b "$BUILD_TARGET" \
  "${MODE_DEFINES[@]}" \
  "${DEBUG_DEFINES[@]}"

# Copy the produced .efi into dist/ for downstream tooling.
EFI_OUT="Build/GblChainload/${BUILD_TARGET}_${TOOLCHAIN_TAG}/${ARCH}/GblChainload.efi"
if [[ ! -f "$EFI_OUT" ]]; then
  echo "error: expected output not found at $EFI_OUT" >&2
  echo "       searching for any GblChainload.efi:" >&2
  find Build -name 'GblChainload.efi' -print 2>&1 | head -5 >&2 || true
  exit 1
fi

cp "$EFI_OUT" "dist/$GBL_MODE_ARTIFACT"
cp "$EFI_OUT" dist/gbl-chainload.efi
echo ">>> done: dist/$GBL_MODE_ARTIFACT and dist/gbl-chainload.efi"
ls -l "dist/$GBL_MODE_ARTIFACT" dist/gbl-chainload.efi
