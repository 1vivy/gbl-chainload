#!/usr/bin/env bash
# tests/host/068_config_table_override.sh
set -euo pipefail
cd "$(dirname "$0")/../.."

make -s -C tests/host/helpers locate_overlay_host
OUT=tests/host/.last/068
mkdir -p "$OUT"

if tests/host/helpers/locate_overlay_host >"$OUT/run.log" 2>&1; then
  echo "PASS: 068 config table override"
else
  echo "FAIL: 068"
  cat "$OUT/run.log"
  exit 1
fi
