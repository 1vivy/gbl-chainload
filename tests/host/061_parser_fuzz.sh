#!/usr/bin/env bash
# tests/host/061_parser_fuzz.sh — corrupt at known positions, expect rejection.
set -euo pipefail
cd "$(dirname "$0")/../.."

PE=images/pe/infiniti-EU-16.0.5.703.efi
[ -f "$PE" ] || { echo "SKIP: $PE missing"; exit 0; }

make -s -C tools/abl-patcher
make -s -C tools/gbl-pack
make -s -C tests/host/helpers parser_harness poison_byte

OUT=tests/host/.last/061
mkdir -p "$OUT"

# Pre-patch the fixture so gbl-pack accepts it.
tools/abl-patcher/abl-patcher --in "$PE" --out "$OUT/patched.efi" 2>"$OUT/patcher.log"

# Pack a clean container.
tools/gbl-pack/gbl-pack --cached-abl "$OUT/patched.efi" --source "$PE" --extracted "$PE" \
  --out "$OUT/clean.bin" 2>/dev/null

# Each (offset, xor, expected_status_label) — values must match
# enum gbl_payload_status numbering in PayloadParse.h.
fuzz() {
  local off=$1 xor=$2 expected=$3 label=$4
  cp "$OUT/clean.bin" "$OUT/poisoned.bin"
  tests/host/helpers/poison_byte "$OUT/poisoned.bin" "$off" "$xor"
  local rc
  tests/host/helpers/parser_harness find-cached-abl "$OUT/poisoned.bin" >"$OUT/run.log" || true
  rc=$(grep -oE 'status=[0-9]+' "$OUT/run.log" | cut -d= -f2)
  if [ "$rc" != "$expected" ]; then
    echo "FAIL: $label (off=$off xor=$xor) expected status=$expected got status=$rc"
    return 1
  fi
  echo "  ok: $label -> status=$rc"
}

# Magic byte 0 -> bad magic (status 2)
fuzz 0 0xFF 2 "magic"
# Version field at offset 8 -> bad version (status 3)
fuzz 8 0xFF 3 "version"
# Header CRC at offset 24 -> CRC mismatch (status 8)
fuzz 24 0xFF 8 "header_crc32"
# Footer at total_size-8 (read total from header bytes [16..20))
TOTAL=$(od -An -tu4 -N4 -j16 "$OUT/clean.bin" | tr -d ' ')
fuzz $((TOTAL - 8)) 0xFF 9 "footer"

echo "PASS: 061 parser fuzz"
