#!/usr/bin/env bash
# scripts/build-cross-tools.sh — cross-compile the six host-side tools for
# Windows and/or macOS inside the docker build image. Outputs to
# dist/windows/ (<tool>.exe) and dist/macos/ (universal <tool>).
#
#   build-cross-tools.sh windows | macos | all
#
# Sibling of build-recovery-tools.sh (which builds the aarch64 Android
# tools). dist/ is git-ignored — these binaries are built on demand.
set -euo pipefail
cd "$(dirname "$0")/.."

OS="${1:-}"
case "$OS" in
  windows|macos|all) ;;
  *) echo "usage: $0 windows|macos|all" >&2; exit 2 ;;
esac

# WSL/Docker-Desktop credential-helper quirk: an empty DOCKER_CONFIG dir
# avoids the desktop.exe credstore lookup that fails under WSL.
export DOCKER_CONFIG="${DOCKER_CONFIG:-$(mktemp -d)}"

TOOLS="fv-unwrap abl-patcher gbl-pack gbl-commit vbmeta-graft mode2-profile"

docker run --rm -v "$PWD:/work" -w /work gbl-chainload-build:latest bash -c '
  set -e
  OS="'"$OS"'"
  TOOLS="'"$TOOLS"'"
  if [ "$OS" = windows ] || [ "$OS" = all ]; then
    mkdir -p dist/windows
    for t in $TOOLS; do
      make -C tools/$t clean
      make -C tools/$t windows
      install -Dm755 tools/$t/$t.exe dist/windows/$t.exe
    done
    ( cd dist/windows && sha256sum *.exe > SHA256SUMS )
  fi
  if [ "$OS" = macos ] || [ "$OS" = all ]; then
    mkdir -p dist/macos
    for t in $TOOLS; do
      make -C tools/$t clean
      make -C tools/$t macos-x64
      make -C tools/$t macos-arm64
      llvm-lipo -create -output dist/macos/$t \
        tools/$t/$t-macos-x64 tools/$t/$t-macos-arm64
    done
    ( cd dist/macos && sha256sum $TOOLS > SHA256SUMS )
  fi
'

echo "==> cross-build done"
[ -d dist/windows ] && ls -la dist/windows
[ -d dist/macos ]   && ls -la dist/macos
exit 0
