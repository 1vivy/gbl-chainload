#!/usr/bin/env bash
# scripts/build.sh — wrap docker EDK-II build with mode/flag selection.
#
# Usage: scripts/build.sh --mode {0|1} [--auto] [--debug] [--verbose] [--cache-abl <abl.img>]
#
# Output: dist/mode-<N>[flags].efi
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

MODE=1
AUTO=0
DEBUG=0
VERBOSE=0
CACHE_ABL=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode)    MODE="$2";    shift 2 ;;
    --auto)    AUTO=1;       shift   ;;
    --debug)   DEBUG=1;      shift   ;;
    --verbose) VERBOSE=1;    shift   ;;
    --cache-abl) CACHE_ABL="$2"; shift 2 ;;
    -h|--help)
      cat <<EOF
Usage: $0 --mode {0|1} [--auto] [--debug] [--verbose] [--cache-abl <abl.img>]

Mode 0: honest unlocked observation + universal preservation baseline; no fakelock overlay.
Mode 1: fakelocked chainload (default).

--cache-abl embeds an already host-patched cached ABL PE into the EFI. At
runtime, gbl-chainload uses that cached PE and deliberately skips dynamic ABL
patching for it.
EOF
      exit 0 ;;
    *) echo "unknown flag: $1" >&2; exit 2 ;;
  esac
done

case "$MODE" in
  0|1) ;;
  *) echo "--mode $MODE not yet supported (valid: 0, 1)" >&2; exit 2 ;;
esac

# Artifact name reflects active flags. This same string is also passed to the
# in-container build as GBL_BUILD_NAME so the EFI publishes it via getvar
# build-name — scripts can identify what's running on device without parsing
# the binary or filename.
SUFFIX=""
[[ $AUTO    -eq 1 ]] && SUFFIX+="-auto"
[[ $DEBUG   -eq 1 ]] && SUFFIX+="-debug"
[[ $VERBOSE -eq 1 ]] && SUFFIX+="-verbose"
[[ -n "$CACHE_ABL" ]] && SUFFIX+="-cache-abl"
BUILD_NAME="mode-${MODE}${SUFFIX}"
ARTIFACT="dist/${BUILD_NAME}.efi"

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

echo "==> Cleaning up previous build caches"
rm -rf Build/

mkdir -p dist Build

GBL_HAS_CACHED_ABL=0
GBL_CACHE_ABL_INPUT=""
if [[ -n "$CACHE_ABL" ]]; then
  if [[ ! -f "$CACHE_ABL" ]]; then
    echo "error: --cache-abl input not found: $CACHE_ABL" >&2
    exit 2
  fi
  CACHE_SIZE=$(stat -c%s "$CACHE_ABL")
  if [[ "$CACHE_SIZE" -le 0 || "$CACHE_SIZE" -gt $((4 * 1024 * 1024)) ]]; then
    echo "error: --cache-abl input size must be >0 and <=4MiB (got $CACHE_SIZE)" >&2
    exit 2
  fi
  mkdir -p Build/cache-abl
  cp "$CACHE_ABL" Build/cache-abl/source-abl.img
  GBL_HAS_CACHED_ABL=1
  GBL_CACHE_ABL_INPUT=/work/Build/cache-abl/source-abl.img
fi

echo "==> Building $ARTIFACT (mode=$MODE auto=$AUTO debug=$DEBUG verbose=$VERBOSE cache_abl=$GBL_HAS_CACHED_ABL)"

# Run the in-container build. Mount repo at /work.
"$DOCKER" run --rm \
  -v "$REPO_ROOT:/work" \
  -w /work \
  --user "$(id -u):$(id -g)" \
  -e GBL_MODE="$MODE" \
  -e GBL_AUTO="$AUTO" \
  -e GBL_DEBUG="$DEBUG" \
  -e GBL_VERBOSE="$VERBOSE" \
  -e GBL_BUILD_NAME="$BUILD_NAME" \
  -e GBL_HAS_CACHED_ABL="$GBL_HAS_CACHED_ABL" \
  -e GBL_CACHE_ABL_INPUT="$GBL_CACHE_ABL_INPUT" \
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
ARTIFACT_SIZE=$(stat -c%s "$ARTIFACT")
if [[ "$GBL_HAS_CACHED_ABL" -eq 1 && "$ARTIFACT_SIZE" -gt 3145728 ]]; then
  echo "ERROR: cache-ABL artifact exceeds 3MiB EFISP size assumption ($ARTIFACT_SIZE bytes)" >&2
  exit 1
fi
echo "==> Built $ARTIFACT (${ARTIFACT_SIZE} bytes)"
