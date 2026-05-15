#!/usr/bin/env bash
# tests/host/063_pe_sanity.sh — exercise PE sanity unit test.
set -euo pipefail
cd "$(dirname "$0")/../.."

make -s -C tests/host/helpers test_pe_sanity
OUT=tests/host/.last/063
mkdir -p "$OUT"
if tests/host/helpers/test_pe_sanity >"$OUT/run.log" 2>&1; then
  echo "PASS: 063 pe_sanity"
else
  echo "FAIL: 063 pe_sanity"
  cat "$OUT/run.log"
  exit 1
fi
