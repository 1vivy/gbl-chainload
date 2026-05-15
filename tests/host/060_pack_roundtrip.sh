#!/usr/bin/env bash
# tests/host/060_pack_roundtrip.sh — pack→parse roundtrip against a real PE.
# The raw fixture contains UTF-16 LE 'efisp' bytes that gbl-pack rejects,
# so we run abl-patcher first to zero those bytes before packing.
set -euo pipefail
cd "$(dirname "$0")/../.."

PE=images/pe/infiniti-EU-16.0.5.703.efi
[ -f "$PE" ] || { echo "SKIP: $PE missing — run scripts/extract-pe-from-fv.sh first" >&2; exit 0; }

make -s -C tools/abl-patcher
make -s -C tools/gbl-pack
make -s -C tests/host/helpers parser_harness

OUT=tests/host/.last/060
mkdir -p "$OUT"

# Pre-patch the fixture so it passes gbl-pack's efisp-scan gate.
# abl-patcher uses --in and --out flags.
tools/abl-patcher/abl-patcher --in "$PE" --out "$OUT/patched.efi" >"$OUT/patcher.log" 2>&1 \
  || { echo "FAIL: abl-patcher failed"; cat "$OUT/patcher.log"; exit 1; }

tools/gbl-pack/gbl-pack \
  --cached-abl "$OUT/patched.efi" --source "$PE" --extracted "$PE" \
  --out "$OUT/payload.bin" 2>"$OUT/pack.log" \
  || { echo "FAIL: gbl-pack failed"; cat "$OUT/pack.log"; exit 1; }

# Parse via parser_harness — header validation
tests/host/helpers/parser_harness parse-header "$OUT/payload.bin" >"$OUT/parse-header.log"
grep -q 'status=0' "$OUT/parse-header.log" \
  || { echo "FAIL: parse-header returned non-zero"; cat "$OUT/parse-header.log"; exit 1; }

# Parse via parser_harness — find cached_abl
tests/host/helpers/parser_harness find-cached-abl "$OUT/payload.bin" >"$OUT/find.log"
grep -q 'status=0' "$OUT/find.log" \
  || { echo "FAIL: find-cached-abl returned non-zero"; cat "$OUT/find.log"; exit 1; }

# Cached size in payload should equal patched PE size (= raw size; patcher zeroes in place).
PATCHED_SIZE=$(stat -c%s "$OUT/patched.efi")
GOT_SIZE=$(grep -oE 'size=[0-9]+' "$OUT/find.log" | cut -d= -f2)
[ "$PATCHED_SIZE" = "$GOT_SIZE" ] \
  || { echo "FAIL: size mismatch patched=$PATCHED_SIZE got=$GOT_SIZE"; exit 1; }

echo "PASS: 060 pack roundtrip"
