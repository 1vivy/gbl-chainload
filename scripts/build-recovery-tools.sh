#!/usr/bin/env bash
# scripts/build-recovery-tools.sh — build the aarch64-Android recovery
# binary inside the docker build image. Outputs dist/recovery/gbl.
#
# PR2 Task 8: the 7 host C tools collapsed into the `gbl` multicall
# binary, so this script now builds one Rust target.
set -euo pipefail
cd "$(dirname "$0")/.."

# WSL/Docker-Desktop credential-helper quirk: an empty DOCKER_CONFIG dir
# avoids the desktop.exe credstore lookup that fails under WSL.
export DOCKER_CONFIG="${DOCKER_CONFIG:-$(mktemp -d)}"

mkdir -p dist/recovery

docker run --rm -v "$PWD:/work" -w /work gbl-chainload-build:latest bash -c '
  set -e
  cargo build --release --locked --target aarch64-linux-android -p gbl
  install -Dm755 target/aarch64-linux-android/release/gbl dist/recovery/gbl
  cd dist/recovery && sha256sum gbl > SHA256SUMS
'

ls -la dist/recovery/
