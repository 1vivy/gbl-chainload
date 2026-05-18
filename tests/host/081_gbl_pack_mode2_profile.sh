#!/usr/bin/env bash
# tests/host/081_gbl_pack_mode2_profile.sh — gbl-pack --mode2-profile builds a
# profile-only GBLP1 0x0010 container the EDK2 parser can locate.
set -euo pipefail
cd "$(dirname "$0")/../.."

make -s -C tools/gbl-pack
make -s -C tests/host/helpers parser_harness
GP=tools/gbl-pack/gbl-pack
H=tests/host/helpers/parser_harness
OUT=tests/host/.last/081
mkdir -p "$OUT"

# A valid 120-byte mode2_profile binary: GM2P, ver 1, reserved 0, then fields.
python3 - "$OUT/profile.bin" <<'PY'
import struct, sys
b  = b"GM2P" + struct.pack("<HHIIII", 1, 0, 0, 0, 0x40000, 0x9A4)
b += bytes(96)   # rot_digest + pubkey_digest + vbh
assert len(b) == 120, len(b)
open(sys.argv[1], "wb").write(b)
PY

# Profile-only container.
"$GP" --mode2-profile "$OUT/profile.bin" --out "$OUT/overlay.bin" 2>"$OUT/pack.log" \
  || { echo "FAIL: gbl-pack --mode2-profile failed"; cat "$OUT/pack.log"; exit 1; }
"$H" find-mode2-profile "$OUT/overlay.bin" | grep -q 'status=0' \
  || { echo "FAIL: 0x0010 entry not locatable in profile-only container"; exit 1; }

# Wrong-size profile -> rejected.
head -c 119 "$OUT/profile.bin" > "$OUT/short.bin"
"$GP" --mode2-profile "$OUT/short.bin" --out "$OUT/bad.bin" >/dev/null 2>&1 \
  && { echo "FAIL: gbl-pack accepted a 119-byte profile"; exit 1; } || true

# Bad magic -> rejected.
python3 - "$OUT/badmagic.bin" "$OUT/profile.bin" <<'PY'
import sys
b = bytearray(open(sys.argv[2],"rb").read()); b[0]=ord('X')
open(sys.argv[1],"wb").write(b)
PY
"$GP" --mode2-profile "$OUT/badmagic.bin" --out "$OUT/bad.bin" >/dev/null 2>&1 \
  && { echo "FAIL: gbl-pack accepted a bad-magic profile"; exit 1; } || true

# Neither input -> usage error.
"$GP" --out "$OUT/bad.bin" >/dev/null 2>&1 \
  && { echo "FAIL: gbl-pack accepted no inputs"; exit 1; } || true

echo "PASS: 081 gbl-pack mode2 profile"
