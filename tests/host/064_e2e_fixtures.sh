#!/usr/bin/env bash
# tests/host/064_e2e_fixtures.sh — pack each tests/images/pe/*.efi fixture (after
# abl-patcher pre-patch) and verify it parses cleanly.
set -euo pipefail
cd "$(dirname "$0")/../.."

# Reproducible gbl-pack output (see 060_pack_roundtrip.sh).
: "${SOURCE_DATE_EPOCH:=0}"
export SOURCE_DATE_EPOCH

cargo build --release --quiet -p gbl
PATH="$PWD/target/release:$PATH"; export PATH
make -s -C tests/host/helpers parser_harness

OUT=tests/host/.last/064
mkdir -p "$OUT"

shopt -s nullglob
fixtures=(tests/images/pe/*.efi)
[ ${#fixtures[@]} -gt 0 ] || { echo "SKIP: no tests/images/pe/*.efi fixtures"; exit 0; }

for pe in "${fixtures[@]}"; do
  name=$(basename "$pe" .efi)
  patched="$OUT/$name.patched.efi"
  gbl patch --in "$pe" --out "$patched" \
    >"$OUT/$name.patcher.log" 2>&1 \
    || { echo "FAIL: $name gbl patch"; cat "$OUT/$name.patcher.log"; exit 1; }
  gbl pack --cached-abl "$patched" --source "$pe" --extracted "$pe" \
    --out "$OUT/$name.bin" 2>"$OUT/$name.pack.log" \
    || { echo "FAIL: $name pack"; cat "$OUT/$name.pack.log"; exit 1; }
  tests/host/helpers/parser_harness find-cached-abl "$OUT/$name.bin" \
    >"$OUT/$name.parse.log" 2>&1
  grep -q 'status=0' "$OUT/$name.parse.log" \
    || { echo "FAIL: $name parse"; cat "$OUT/$name.parse.log"; exit 1; }
  echo "  ok: $name"
done

echo "PASS: 064 e2e fixtures"
