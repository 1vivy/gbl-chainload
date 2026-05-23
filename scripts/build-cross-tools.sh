#!/usr/bin/env bash
# scripts/build-cross-tools.sh — cross-compile the `gbl` multicall binary
# for Windows, macOS, and/or Linux inside the docker build image. Outputs
# to dist/windows/gbl.exe, dist/macos/gbl (universal), and dist/linux/gbl
# (static x86_64 ELF).
#
#   build-cross-tools.sh windows | macos | linux | all
#
# Sibling of build-recovery-tools.sh (which builds the aarch64 Android
# target). dist/ is git-ignored — these binaries are built on demand.
#
# PR2 Task 8: the 7 host C tools collapsed into the `gbl` multicall, so
# this script now builds one Rust target per platform rather than seven
# C executables.
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

docker run --rm -v "$PWD:/work" -w /work gbl-chainload-build:latest bash -c '
  set -e
  OS="'"$OS"'"
  if [ "$OS" = windows ] || [ "$OS" = all ]; then
    cargo build --release --locked --target x86_64-pc-windows-gnu -p gbl
    mkdir -p dist/windows
    install -Dm755 target/x86_64-pc-windows-gnu/release/gbl.exe dist/windows/gbl.exe
    ( cd dist/windows && sha256sum *.exe > SHA256SUMS )
  fi
  if [ "$OS" = macos ] || [ "$OS" = all ]; then
    cargo build --release --locked --target x86_64-apple-darwin -p gbl
    cargo build --release --locked --target aarch64-apple-darwin -p gbl
    mkdir -p dist/macos
    llvm-lipo -create -output dist/macos/gbl \
      target/x86_64-apple-darwin/release/gbl \
      target/aarch64-apple-darwin/release/gbl
    chmod +x dist/macos/gbl
    ( cd dist/macos && sha256sum gbl > SHA256SUMS )
  fi
  if [ "$OS" = linux ] || [ "$OS" = all ]; then
    cargo build --release --locked --target x86_64-unknown-linux-musl -p gbl
    mkdir -p dist/linux
    install -Dm755 target/x86_64-unknown-linux-musl/release/gbl dist/linux/gbl
    ( cd dist/linux && sha256sum gbl > SHA256SUMS )
  fi
'

echo "==> cross-build done"
[ -d dist/windows ] && ls -la dist/windows
[ -d dist/macos ]   && ls -la dist/macos
[ -d dist/linux ]   && ls -la dist/linux
exit 0
