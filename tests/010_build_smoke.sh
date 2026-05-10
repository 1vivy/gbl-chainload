#!/usr/bin/env bash
# 010_build_smoke.sh — verify scripts/build.sh produces the expected artifacts.
set -euo pipefail
cd "$(dirname "$0")/.."

echo "== building dist/mode-0.efi =="
./scripts/build.sh --mode 0
test -f dist/mode-0.efi || { echo "FAIL: dist/mode-0.efi missing"; exit 1; }

echo "== building dist/mode-1.efi =="
./scripts/build.sh --mode 1
test -f dist/mode-1.efi || { echo "FAIL: dist/mode-1.efi missing"; exit 1; }

echo "== building dist/mode-1-auto-debug-verbose.efi =="
./scripts/build.sh --mode 1 --auto --debug --verbose
test -f dist/mode-1-auto-debug-verbose.efi \
  || { echo "FAIL: dist/mode-1-auto-debug-verbose.efi missing"; exit 1; }

echo "ok 010_build_smoke"
