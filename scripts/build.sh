#!/usr/bin/env bash
# scripts/build.sh — wrap docker EDK-II build with mode/flag selection.
#
# Usage: scripts/build.sh --mode {1|2|3} [--auto] [--debug] [--verbose]
#                         [--embed <abl.bin>] [--profile <name>]
#
# Output: dist/mode-<N>[flags].efi
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

MODE=1
AUTO=0
DEBUG=0
VERBOSE=0
EMBED=""        # Plan 3 placeholder
PROFILE=""      # Plan 2 placeholder

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode)    MODE="$2";    shift 2 ;;
    --auto)    AUTO=1;       shift   ;;
    --debug)   DEBUG=1;      shift   ;;
    --verbose) VERBOSE=1;    shift   ;;
    --embed)   EMBED="$2";   shift 2 ;;
    --profile) PROFILE="$2"; shift 2 ;;
    -h|--help)
      cat <<EOF
Usage: $0 --mode {1|2|3} [--auto] [--debug] [--verbose]
              [--embed <abl.bin>] [--profile <dev>/<ota>]

Plan-1 supports --mode 1 only; mode 2/3 land in plans 2/3.
--embed and --profile are placeholders for plan 3 / plan 2.
EOF
      exit 0 ;;
    *) echo "unknown flag: $1" >&2; exit 2 ;;
  esac
done

case "$MODE" in
  1) ;;
  2|3) echo "WARN: mode $MODE not yet supported in plan 1; building anyway" >&2 ;;
  *) echo "--mode must be 1, 2, or 3" >&2; exit 2 ;;
esac

# Artifact name reflects active flags.
SUFFIX=""
[[ $AUTO    -eq 1 ]] && SUFFIX+="-auto"
[[ $DEBUG   -eq 1 ]] && SUFFIX+="-debug"
[[ $VERBOSE -eq 1 ]] && SUFFIX+="-verbose"
ARTIFACT="dist/mode-${MODE}${SUFFIX}.efi"

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

mkdir -p dist Build

echo "==> Building $ARTIFACT (mode=$MODE auto=$AUTO debug=$DEBUG verbose=$VERBOSE)"

# Run the in-container build. Mount repo at /work.
"$DOCKER" run --rm \
  -v "$REPO_ROOT:/work" \
  -w /work \
  --user "$(id -u):$(id -g)" \
  -e GBL_MODE="$MODE" \
  -e GBL_AUTO="$AUTO" \
  -e GBL_DEBUG="$DEBUG" \
  -e GBL_VERBOSE="$VERBOSE" \
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
echo "==> Built $ARTIFACT ($(stat -c%s "$ARTIFACT") bytes)"
