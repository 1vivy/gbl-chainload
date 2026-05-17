#!/usr/bin/env bash
# scripts/build-recovery-tools.sh — build all aarch64-Android recovery
# tools inside the docker build image. Outputs to dist/recovery/.
set -euo pipefail
cd "$(dirname "$0")/.."

# WSL/Docker-Desktop credential-helper quirk: an empty DOCKER_CONFIG dir
# avoids the desktop.exe credstore lookup that fails under WSL.
export DOCKER_CONFIG="${DOCKER_CONFIG:-$(mktemp -d)}"

mkdir -p dist/recovery

docker run --rm -v "$PWD:/work" -w /work gbl-chainload-build:latest bash -c '
  set -e
  for t in fv-unwrap abl-patcher gbl-pack gbl-commit; do
    make -C tools/$t clean
    make -C tools/$t android
    install -Dm755 tools/$t/$t-android dist/recovery/$t
  done
  cd dist/recovery && sha256sum * > SHA256SUMS
'

ls -la dist/recovery/
