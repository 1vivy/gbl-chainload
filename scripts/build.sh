#!/usr/bin/env bash
# scripts/build.sh — orchestrate the full gbl-chainload build pipeline.
#
# PR2 Task 9: this script is the single entry point covering the
# four-phase build per docs rust-tooling spec §4:
#
#   1. cargo build the firmware staticlibs (avb-parse, gblp1,
#      mode2-profile-core, patch-engine, pe-utils) for
#      aarch64-unknown-none.
#   2. EDK2 build, which links the staticlibs into
#      dist/gbl-chainload[-suffix].efi.
#   3. cargo cross-build the `gbl` multicall for aarch64-linux-android
#      (recovery), producing dist/recovery/gbl.
#   4. cargo build the `gbl` multicall for the host (native), producing
#      dist/host/gbl.
#
# Phases 1–3 happen inside the gbl-chainload-build:latest docker image
# (same image scripts/build-inside-docker.sh and scripts/build-recovery-
# tools.sh use); phase 4 runs natively on the host. Cross-building for
# Windows / macOS is handled separately by scripts/build-cross-tools.sh
# (test 084) and intentionally not part of the default pipeline — it
# requires non-trivial linker toolchains.
#
# Engine rework (Task 11): the per-mode compile flag is gone. Activation
# is manifest-driven at runtime, so one EFI handles every install
# profile.
#
# Usage: scripts/build.sh [--auto] [--debug] [--verbose] [--no-recovery] [--no-host]
#
# Outputs:
#   dist/gbl-chainload[-suffix].efi   (firmware; suffix from --auto/--debug/--verbose)
#   dist/recovery/gbl                  (aarch64-linux-android multicall)
#   dist/host/gbl                      (native multicall; pass --no-host to skip)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

AUTO=0
DEBUG=0
VERBOSE=0
DO_RECOVERY=1
DO_HOST=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    --auto)         AUTO=1;          shift ;;
    --debug)        DEBUG=1;         shift ;;
    --verbose)      VERBOSE=1;       shift ;;
    --no-recovery)  DO_RECOVERY=0;   shift ;;
    --no-host)      DO_HOST=0;       shift ;;
    -h|--help)
      cat <<EOF
Usage: $0 [--auto] [--debug] [--verbose] [--no-recovery] [--no-host]

Orchestrates the full four-phase gbl-chainload build:

  1. firmware staticlibs (cargo aarch64-unknown-none, in docker)
  2. EDK2 link              (in docker; emits dist/gbl-chainload[-suffix].efi)
  3. recovery gbl multicall (cargo aarch64-linux-android, in docker;
                             emits dist/recovery/gbl). Skip with --no-recovery.
  4. host gbl multicall     (cargo host native; emits dist/host/gbl).
                             Skip with --no-host.

Activation of fakelock / profile-spoof behavior is driven by the runtime
GBLP1 manifest baked into the EFISP overlay, not by compile flags. Build
once; the same firmware binary runs every install profile.
EOF
      exit 0 ;;
    *) echo "unknown flag: $1" >&2; exit 2 ;;
  esac
done

# Artifact name reflects active build flags. This same string is also passed
# to the in-container build as GBL_BUILD_NAME so the EFI publishes it via
# getvar gbl-chainload_build — scripts can identify what's running on device
# without parsing the binary or filename.
SUFFIX=""
if [[ $AUTO    -eq 1 ]]; then SUFFIX+="-auto";    fi
if [[ $DEBUG   -eq 1 ]]; then SUFFIX+="-debug";   fi
if [[ $VERBOSE -eq 1 ]]; then SUFFIX+="-verbose"; fi
BUILD_NAME="gbl-chainload${SUFFIX}"
ARTIFACT="dist/${BUILD_NAME}.efi"

# Read version from top-level VERSION file (single source of truth).
if [[ ! -f VERSION ]]; then
  echo "error: VERSION file missing at repo root" >&2
  exit 1
fi
GBL_CHAINLOAD_VERSION="$(head -n1 VERSION | tr -d '[:space:]')"
if [[ -z "$GBL_CHAINLOAD_VERSION" ]]; then
  echo "error: VERSION file at $REPO_ROOT/VERSION is empty (after whitespace strip)" >&2
  exit 1
fi
export GBL_CHAINLOAD_VERSION

IMAGE_TAG="gbl-chainload-build:latest"

if command -v docker >/dev/null 2>&1; then
  DOCKER=docker
elif [[ -x /Applications/Docker.app/Contents/Resources/bin/docker ]]; then
  DOCKER=/Applications/Docker.app/Contents/Resources/bin/docker
else
  echo "error: docker not found in PATH" >&2
  exit 1
fi

# Build the image on demand if it doesn't exist locally.
if ! "$DOCKER" image inspect "$IMAGE_TAG" >/dev/null 2>&1; then
  echo ">>> building $IMAGE_TAG (one-time)"
  "$DOCKER" build -t "$IMAGE_TAG" -f docker/Dockerfile .
fi

echo "==> [1/4 + 2/4] Cleaning up previous EDK2 build caches"
rm -rf Build/

mkdir -p dist Build

echo "==> [1/4 + 2/4] Building firmware staticlibs + $ARTIFACT (auto=$AUTO debug=$DEBUG verbose=$VERBOSE)"

# Phases 1 + 2: run in-container. build-inside-docker.sh cargo-builds
# every aarch64-unknown-none staticlib that the firmware actually links
# (avb-parse, gblp1, mode2-profile-core, patch-engine) in workspace
# order, and then invokes the EDK2 build, which DLINK_FLAGS's them
# into the EFI.
#
# pe-utils has no firmware link consumer today (GblPayloadLib.inf
# notes the staticlib stays unlinked until a firmware call site
# lands), so it is NOT part of the docker firmware-staticlib build.
# It is built transitively as an rlib by phase 4 (the host `gbl`
# multicall, which links it via tools/gbl/Cargo.toml).
"$DOCKER" run --rm \
  -v "$REPO_ROOT:/work" \
  -w /work \
  --user "$(id -u):$(id -g)" \
  -e GBL_AUTO="$AUTO" \
  -e GBL_DEBUG="$DEBUG" \
  -e GBL_VERBOSE="$VERBOSE" \
  -e GBL_BUILD_NAME="$BUILD_NAME" \
  -e GBL_CHAINLOAD_VERSION="$GBL_CHAINLOAD_VERSION" \
  "$IMAGE_TAG" \
  bash scripts/build-inside-docker.sh

# Pick up the EDK-II RELEASE output and copy to dist/ with the artifact name.
# build-inside-docker.sh (running in-container at /work == repo root) writes
# to Build/GblChainloadPkg/... and also copies to dist/gbl-chainload.efi.
EDK_OUT=$(ls "Build/GblChainloadPkg/RELEASE_"*/AARCH64/GblChainload.efi 2>/dev/null | head -1)
if [[ -z "$EDK_OUT" || ! -f "$EDK_OUT" ]]; then
  # Fallback: build-inside-docker.sh also copies to dist/gbl-chainload.efi.
  if [[ -f dist/gbl-chainload.efi ]]; then
    EDK_OUT=dist/gbl-chainload.efi
  else
    echo "ERROR: build did not produce GblChainload.efi" >&2
    exit 1
  fi
fi
cp "$EDK_OUT" "$ARTIFACT"
echo "==> [1/4 + 2/4] Built $ARTIFACT ($(stat -c%s "$ARTIFACT") bytes)"

# Phase 3: cross-build the `gbl` multicall for aarch64-linux-android.
# scripts/build-recovery-tools.sh wraps the same docker image and emits
# dist/recovery/gbl + dist/recovery/SHA256SUMS. PR2 Task 8 already wired
# it to the single multicall target.
if [[ $DO_RECOVERY -eq 1 ]]; then
  echo "==> [3/4] Cross-building recovery gbl multicall (aarch64-linux-android)"
  bash scripts/build-recovery-tools.sh
else
  echo "==> [3/4] SKIP recovery cross-build (--no-recovery)"
fi

# Phase 4: native host build of the `gbl` multicall. We use the host
# toolchain (not docker) so the resulting binary can run on the
# developer's box for local testing without a docker round-trip.
if [[ $DO_HOST -eq 1 ]]; then
  echo "==> [4/4] Building host gbl multicall (native)"
  if ! command -v cargo >/dev/null 2>&1; then
    echo "ERROR: cargo not found on host PATH (host gbl build requires a working Rust toolchain)" >&2
    echo "       Pass --no-host to skip phase 4, or install rustup." >&2
    exit 1
  fi
  cargo build --release --locked -p gbl
  mkdir -p dist/host
  cp target/release/gbl dist/host/gbl
  echo "==> [4/4] Built dist/host/gbl ($(stat -c%s dist/host/gbl) bytes)"
else
  echo "==> [4/4] SKIP host build (--no-host)"
fi

echo "==> done."
echo "    firmware: $ARTIFACT"
if [[ $DO_RECOVERY -eq 1 ]]; then echo "    recovery: dist/recovery/gbl"; fi
if [[ $DO_HOST     -eq 1 ]]; then echo "    host:     dist/host/gbl";     fi
