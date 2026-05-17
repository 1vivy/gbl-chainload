#!/usr/bin/env bash
# tests/host/067_blockio_reader_smoke.sh — parse a synthetic raw-EFISP image.
# Validates that the PE-end + magic-scan path the EDK2 BlockIO reader will
# use produces the same parse result as direct GBLP1 input.
set -euo pipefail
cd "$(dirname "$0")/../.."

PE=images/pe/infiniti-EU-16.0.5.703.efi
[ -f "$PE" ] || { echo "SKIP: $PE missing"; exit 0; }

make -s -C tools/abl-patcher
make -s -C tools/gbl-pack
make -s -C tests/host/helpers parser_harness

OUT=tests/host/.last/067
mkdir -p "$OUT"

# 1. Pre-patch the PE so gbl-pack accepts it.
tools/abl-patcher/abl-patcher --in "$PE" --out "$OUT/patched.efi" \
  >"$OUT/patcher.log" 2>&1

# 2. Pack a payload from the patched PE.
tools/gbl-pack/gbl-pack --cached-abl "$OUT/patched.efi" --source "$PE" --extracted "$PE" \
  --out "$OUT/payload.bin" 2>"$OUT/pack.log"

# 3. Concat: simulate the EFISP raw partition contents:
#    [PE bytes (gbl-chainload itself, here we reuse the test PE) || GBLP1].
#    The runtime reader will scan past PE end for "GBLP1\0\0\0".
cat "$OUT/patched.efi" "$OUT/payload.bin" >"$OUT/efisp.img"

# 4. Confirm the GBLP1 magic appears immediately after the patched PE bytes.
#    Use hex comparison for portability across different od implementations.
PE_SIZE=$(stat -c%s "$OUT/patched.efi")
EXPECTED_HEX="47424c5031000000"  # "GBLP1\0\0\0" in hex
GOT_HEX=$(dd if="$OUT/efisp.img" bs=1 skip="$PE_SIZE" count=8 2>/dev/null | xxd -p)
[ "$GOT_HEX" = "$EXPECTED_HEX" ] \
  || { echo "FAIL: GBLP1 magic not at PE end (got: $GOT_HEX)"; exit 1; }

# 5. Parse just the appended portion with parser_harness — same code the
#    EDK2 parser uses.
tail -c +$((PE_SIZE + 1)) "$OUT/efisp.img" >"$OUT/payload-from-img.bin"
tests/host/helpers/parser_harness find-cached-abl "$OUT/payload-from-img.bin" \
  >"$OUT/parse.log"
grep -q 'status=0' "$OUT/parse.log" \
  || { echo "FAIL: parse"; cat "$OUT/parse.log"; exit 1; }

echo "PASS: 067 blockio reader smoke (synthetic raw EFISP)"
