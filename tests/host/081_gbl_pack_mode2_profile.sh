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

# Combined path: cached_abl + source_meta + mode2_profile (ec=3).
# The PE fixture may not be present on all build hosts; skip only this
# sub-case if it is missing — the rest of 081 has already passed above.
PE=images/pe/infiniti-EU-16.0.5.703.efi
if [ ! -f "$PE" ]; then
  echo "SKIP: $PE missing — combined ec=3 sub-case skipped (rest of 081 passed)"
else
  make -s -C tools/abl-patcher
  tools/abl-patcher/abl-patcher --in "$PE" --out "$OUT/patched.efi" \
    >"$OUT/patcher.log" 2>&1 \
    || { echo "FAIL: abl-patcher failed for ec=3 sub-case"; cat "$OUT/patcher.log"; exit 1; }

  "$GP" --cached-abl "$OUT/patched.efi" --source "$PE" --extracted "$OUT/patched.efi" \
    --mode2-profile "$OUT/profile.bin" --out "$OUT/combined.bin" \
    2>"$OUT/combined-pack.log" \
    || { echo "FAIL: gbl-pack ec=3 combined path failed"; cat "$OUT/combined-pack.log"; exit 1; }

  "$H" find-cached-abl "$OUT/combined.bin" | grep -q 'status=0' \
    || { echo "FAIL: find-cached-abl failed on ec=3 combined container"; exit 1; }

  "$H" find-mode2-profile "$OUT/combined.bin" | grep -q 'status=0' \
    || { echo "FAIL: find-mode2-profile failed on ec=3 combined container"; exit 1; }
fi

echo "PASS: 081 gbl-pack mode2 profile"
