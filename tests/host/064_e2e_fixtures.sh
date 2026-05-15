#!/usr/bin/env bash
# tests/host/064_e2e_fixtures.sh — pack each images/pe/*.efi fixture (after
# abl-patcher pre-patch) and verify it parses cleanly.
set -euo pipefail
cd "$(dirname "$0")/../.."

make -s -C tools/abl-patcher
make -s -C tools/gbl-pack
make -s -C tests/host/helpers parser_harness

OUT=tests/host/.last/064
mkdir -p "$OUT"

shopt -s nullglob
fixtures=(images/pe/*.efi)
[ ${#fixtures[@]} -gt 0 ] || { echo "SKIP: no images/pe/*.efi fixtures"; exit 0; }

for pe in "${fixtures[@]}"; do
  name=$(basename "$pe" .efi)
  patched="$OUT/$name.patched.efi"
  tools/abl-patcher/abl-patcher --in "$pe" --out "$patched" \
    >"$OUT/$name.patcher.log" 2>&1 \
    || { echo "FAIL: $name abl-patcher"; cat "$OUT/$name.patcher.log"; exit 1; }
  tools/gbl-pack/gbl-pack --cached-abl "$patched" --source "$pe" --extracted "$pe" \
    --out "$OUT/$name.bin" 2>"$OUT/$name.pack.log" \
    || { echo "FAIL: $name pack"; cat "$OUT/$name.pack.log"; exit 1; }
  tests/host/helpers/parser_harness find-cached-abl "$OUT/$name.bin" \
    >"$OUT/$name.parse.log" 2>&1
  grep -q 'status=0' "$OUT/$name.parse.log" \
    || { echo "FAIL: $name parse"; cat "$OUT/$name.parse.log"; exit 1; }
  echo "  ok: $name"
done

echo "PASS: 064 e2e fixtures"
