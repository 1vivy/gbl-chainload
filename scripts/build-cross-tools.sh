#!/usr/bin/env bash
# scripts/build-cross-tools.sh — cross-compile the host-side tools for
# Windows, macOS, and/or Linux inside the docker build image. Outputs to
# dist/windows/ (<tool>.exe), dist/macos/ (universal <tool>), and
# dist/linux/ (static x86_64 ELF <tool>).
#
#   build-cross-tools.sh windows | macos | linux | all
#
# Sibling of build-recovery-tools.sh (which builds the aarch64 Android
# tools). dist/ is git-ignored — these binaries are built on demand.
set -euo pipefail
cd "$(dirname "$0")/.."

OS="${1:-}"
case "$OS" in
  windows|macos|linux|all) ;;
  *) echo "usage: $0 windows|macos|linux|all" >&2; exit 2 ;;
esac

# WSL/Docker-Desktop credential-helper quirk: an empty DOCKER_CONFIG dir
# avoids the desktop.exe credstore lookup that fails under WSL.
export DOCKER_CONFIG="${DOCKER_CONFIG:-$(mktemp -d)}"

TOOLS="fv-unwrap abl-patcher gbl-pack gbl-commit vbmeta-graft mode2-profile gblp1-inspect"

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
  if [ "$OS" = linux ] || [ "$OS" = all ]; then
    mkdir -p dist/linux
    for t in $TOOLS; do
      make -C tools/$t clean
      make -C tools/$t linux
      install -Dm755 tools/$t/$t-linux dist/linux/$t
    done
    ( cd dist/linux && sha256sum $TOOLS > SHA256SUMS )
  fi
'

echo "==> cross-build done"
[ -d dist/windows ] && ls -la dist/windows
[ -d dist/macos ]   && ls -la dist/macos
[ -d dist/linux ]   && ls -la dist/linux
exit 0
