#!/usr/bin/env bash
# tests/host/078_mode2_rewrite.sh — mode-2 KM rewrite unit test.
set -euo pipefail
cd "$(dirname "$0")/../.."

make -s -C tests/host/helpers mode2_harness
H=tests/host/helpers/mode2_harness
OUT=tests/host/.last/078
mkdir -p "$OUT"

# Profile: is_unlocked=0, color=0, sysver=0x40000, spl=0x9A4,
# rot_digest=0x11*32, pubkey_digest=0x22*32, vbh=0x33*32.
python3 - "$OUT/profile.bin" <<'PY'
import struct, sys
p  = b"GM2P" + struct.pack("<HHIIII",1,0,0,0,0x40000,0x9A4)
p += b"\x11"*32 + b"\x22"*32 + b"\x33"*32
open(sys.argv[1],"wb").write(p)
PY

# SET_BOOT_STATE buffer (64B): cmd=0x208, rest = honest/unlocked junk.
python3 - "$OUT/bs.bin" <<'PY'
import struct, sys
b  = struct.pack("<IIII", 0x208, 0, 16, 48)        # cmd,Version,Offset,Size
b += struct.pack("<I", 1)                          # IsUnlocked = 1 (honest)
b += b"\xAA"*32                                    # PublicKey (custom)
b += struct.pack("<III", 2, 0, 0)                  # Color=ORANGE,sysver,spl
assert len(b) == 64, len(b)
open(sys.argv[1],"wb").write(b)
PY

OUT_HEX=$("$H" rewrite 0x208 "$OUT/profile.bin" "$OUT/bs.bin")
echo "$OUT_HEX" | grep -q 'rewrote=1' \
  || { echo "FAIL: SET_BOOT_STATE not rewritten"; echo "$OUT_HEX"; exit 1; }

# Last line is the rewritten buffer hex. Verify the spoofed fields:
HEX=$(echo "$OUT_HEX" | tail -1)
# IsUnlocked @16 (bytes 32..39 of hex) must now be 00000000
[ "${HEX:32:8}" = "00000000" ] \
  || { echo "FAIL: IsUnlocked not zeroed (${HEX:32:8})"; exit 1; }
# PublicKey @20 (hex 40..103) must be 0x22 * 32
[ "${HEX:40:64}" = "$(printf '22%.0s' {1..32})" ] \
  || { echo "FAIL: PublicKey not rewritten"; exit 1; }
# Color @52 (hex 104..111) must be 00000000 (GREEN)
[ "${HEX:104:8}" = "00000000" ] \
  || { echo "FAIL: Color not GREEN (${HEX:104:8})"; exit 1; }

# Wrong length must be rejected: a 63-byte SET_BOOT_STATE -> rewrote=0.
head -c 63 "$OUT/bs.bin" > "$OUT/bs_short.bin"
"$H" rewrite 0x208 "$OUT/profile.bin" "$OUT/bs_short.bin" | grep -q 'rewrote=0' \
  || { echo "FAIL: short SET_BOOT_STATE was rewritten"; exit 1; }

# Non-target cmd-id (0x219) must never be rewritten.
"$H" rewrite 0x219 "$OUT/profile.bin" "$OUT/bs.bin" | grep -q 'rewrote=0' \
  || { echo "FAIL: 0x219 was rewritten"; exit 1; }

echo "PASS: 078 mode2 rewrite"
