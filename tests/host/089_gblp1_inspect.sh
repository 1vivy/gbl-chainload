#!/usr/bin/env bash
# tests/host/089_gblp1_inspect.sh — gblp1-inspect round-trip + failure-mode.
set -euo pipefail
cd "$(dirname "$0")/../.."

PE=tests/images/pe/infiniti-EU-16.0.5.703.efi
[ -f "$PE" ] || { echo "SKIP: $PE missing" >&2; exit 0; }

make -s -C tools/abl-patcher
make -s -C tools/gbl-pack
make -s -C tools/gblp1-inspect

OUT=tests/host/.last/089
rm -rf "$OUT"; mkdir -p "$OUT"

# Build a valid payload.bin via the same path as 060.
tools/abl-patcher/abl-patcher --in "$PE" --out "$OUT/patched.efi" >/dev/null
tools/gbl-pack/gbl-pack --cached-abl "$OUT/patched.efi" --source "$PE" \
                       --extracted "$PE" --out "$OUT/payload.bin"

# 1. Happy path on a bare payload.
tools/gblp1-inspect/gblp1-inspect "$OUT/payload.bin" > "$OUT/ok.txt"
grep -q '^result: ok$' "$OUT/ok.txt" \
  || { echo "FAIL: clean payload did not produce result: ok"; cat "$OUT/ok.txt"; exit 1; }

# 2. Happy path with arbitrary leading bytes (simulating EFISP = base EFI || GBLP1).
head -c 65536 /dev/urandom > "$OUT/prefix.bin"
cat "$OUT/prefix.bin" "$OUT/payload.bin" > "$OUT/efisp-like.img"
tools/gblp1-inspect/gblp1-inspect "$OUT/efisp-like.img" > "$OUT/ok-prefixed.txt"
grep -q '^result: ok$' "$OUT/ok-prefixed.txt" \
  || { echo "FAIL: prefixed payload did not produce result: ok"; cat "$OUT/ok-prefixed.txt"; exit 1; }

# 3. Corrupt a single entry's payload (flip one byte at offset 0x100, which is inside the
#    first entry payload region given header+entries footprint).
cp "$OUT/payload.bin" "$OUT/corrupt.bin"
python3 -c '
import sys
p=open(sys.argv[1],"r+b"); p.seek(0x100); b=p.read(1); p.seek(0x100); p.write(bytes([b[0]^0xff])); p.close()
' "$OUT/corrupt.bin"
set +e
tools/gblp1-inspect/gblp1-inspect "$OUT/corrupt.bin" > "$OUT/corrupt.txt"
rc=$?
set -e
grep -q '^result: entry_sha_mismatch$' "$OUT/corrupt.txt" \
  || { echo "FAIL: corrupt payload did not produce entry_sha_mismatch"; cat "$OUT/corrupt.txt"; exit 1; }
[ "$rc" != 0 ] \
  || { echo "FAIL: corrupt input returned exit 0"; exit 1; }

# 4. Not-a-gblp1: feed pure random.
head -c 4096 /dev/urandom > "$OUT/random.bin"
set +e
tools/gblp1-inspect/gblp1-inspect "$OUT/random.bin" > "$OUT/random.txt"
rc=$?
set -e
grep -q '^result: not_a_gblp1$' "$OUT/random.txt" \
  || { echo "FAIL: random input did not produce not_a_gblp1"; cat "$OUT/random.txt"; exit 1; }
[ "$rc" != 0 ] \
  || { echo "FAIL: not_a_gblp1 returned exit 0"; exit 1; }

echo "PASS: 089 gblp1-inspect"
