#!/usr/bin/env bash
# tests/host/069_full_buffer_scan.sh — parse a full [PE | GBLP1] concat from
# offset 0, the way the runtime does. Regression test for the embedded-magic
# false-match bug: the gbl-chainload PE contains the GBLP1 magic string
# literal in .rodata; a naive first-match scan finds that instead of the
# real appended container.
set -euo pipefail
cd "$(dirname "$0")/../.."

PE=images/pe/infiniti-EU-16.0.5.703.efi
[ -f "$PE" ] || { echo "SKIP: $PE missing"; exit 0; }

make -s -C tools/abl-patcher
make -s -C tools/gbl-pack
make -s -C tests/host/helpers parser_harness

OUT=tests/host/.last/069
mkdir -p "$OUT"

# Pre-patch + pack a real GBLP1 payload.
tools/abl-patcher/abl-patcher --in "$PE" --out "$OUT/patched.efi" 2>/dev/null
tools/gbl-pack/gbl-pack --cached-abl "$OUT/patched.efi" --source "$PE" \
  --extracted "$PE" --out "$OUT/payload.bin" 2>/dev/null

# Build a stand-in "PE" prefix that DELIBERATELY contains the GBLP1 magic,
# to mimic gbl-chainload's own .rodata. Use the parser_harness binary itself
# as the prefix — it links GBLP1_MAGIC so it contains the literal — OR
# synthesize one. Simplest + faithful: prepend a blob that embeds the magic.
# Here: use the host parser_harness binary as the PE stand-in (it embeds the
# magic exactly as the real EFI does).
PREFIX=tests/host/helpers/parser_harness
cat "$PREFIX" "$OUT/payload.bin" > "$OUT/full.bin"

# Sanity: the prefix really does contain the magic (else the test is moot).
if ! xxd -p "$PREFIX" | tr -d '\n' | grep -q 47424c5031000000; then
  echo "SKIP: prefix has no embedded magic — test would not exercise the bug"
  exit 0
fi

# Scan the FULL buffer from offset 0 — must still find the real container.
tests/host/helpers/parser_harness scan-cached-abl "$OUT/full.bin" >"$OUT/scan.log" 2>&1
grep -q 'status=0' "$OUT/scan.log" \
  || { echo "FAIL: scan-cached-abl did not find the real container"; cat "$OUT/scan.log"; exit 1; }

echo "PASS: 069 full-buffer scan (embedded-magic tolerant)"
