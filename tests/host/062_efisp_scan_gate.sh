#!/usr/bin/env bash
# tests/host/062_efisp_scan_gate.sh — gbl-pack must refuse a poisoned PE.
set -euo pipefail
cd "$(dirname "$0")/../.."

PE=images/pe/infiniti-EU-16.0.5.703.efi
[ -f "$PE" ] || { echo "SKIP: $PE missing"; exit 0; }

make -s -C tools/abl-patcher
make -s -C tools/gbl-pack
make -s -C tests/host/helpers inject_efisp

OUT=tests/host/.last/062
mkdir -p "$OUT"

# Pre-patch the fixture so it passes gbl-pack's efisp gate.
tools/abl-patcher/abl-patcher --in "$PE" --out "$OUT/patched.efi" 2>"$OUT/patcher.log"

# Sanity: clean PATCHED PE packs OK.
tools/gbl-pack/gbl-pack --cached-abl "$OUT/patched.efi" --source "$PE" --extracted "$PE" \
  --out "$OUT/clean.bin" 2>/dev/null

# Poison a copy of the PATCHED PE and assert pack refuses.
cp "$OUT/patched.efi" "$OUT/poisoned.efi"
POISONED_SIZE=$(stat -c%s "$OUT/poisoned.efi")
tests/host/helpers/inject_efisp "$OUT/poisoned.efi" $((POISONED_SIZE / 2))

if tools/gbl-pack/gbl-pack --cached-abl "$OUT/poisoned.efi" --source "$PE" \
                           --extracted "$PE" --out "$OUT/should-not-exist.bin" \
                           2>"$OUT/pack.log"; then
  echo "FAIL: gbl-pack accepted a poisoned PE"
  cat "$OUT/pack.log"
  exit 1
fi
grep -q 'status=1' "$OUT/pack.log" \
  || { echo "FAIL: wrong reject status"; cat "$OUT/pack.log"; exit 1; }

echo "PASS: 062 efisp scan gate"
