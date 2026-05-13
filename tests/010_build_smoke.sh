#!/usr/bin/env bash
# 010_build_smoke.sh — verify scripts/build.sh produces the expected artifacts.
#
# Requires docker (scripts/build.sh runs EDK-II inside a container). If docker
# is unavailable on the runner, skip cleanly — host-side lint/scan/patch tests
# still run, the EFI compile path simply isn't validated here.
set -euo pipefail
cd "$(dirname "$0")/.."

if ! command -v docker >/dev/null 2>&1; then
  echo "SKIP: 010_build_smoke — docker not in PATH on this runner"
  echo "  (PATH=$PATH)"
  ls -la /usr/bin/docker 2>&1 | head -1 || true
  exit 0
fi

echo "== building dist/mode-0.efi =="
./scripts/build.sh --mode 0
test -f dist/mode-0.efi || { echo "FAIL: dist/mode-0.efi missing"; exit 1; }

echo "== building dist/mode-1.efi =="
./scripts/build.sh --mode 1
test -f dist/mode-1.efi || { echo "FAIL: dist/mode-1.efi missing"; exit 1; }

echo "== building dist/mode-1-auto-debug.efi =="
./scripts/build.sh --mode 1 --auto --debug
test -f dist/mode-1-auto-debug.efi \
  || { echo "FAIL: dist/mode-1-auto-debug.efi missing"; exit 1; }

echo "ok 010_build_smoke"
