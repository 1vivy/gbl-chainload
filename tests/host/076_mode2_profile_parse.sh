#!/usr/bin/env bash
# tests/host/076_mode2_profile_parse.sh — mode2_profile parser unit test.
set -euo pipefail
cd "$(dirname "$0")/../.."

make -s -C tests/host/helpers mode2_harness
H=tests/host/helpers/mode2_harness
OUT=tests/host/.last/076
mkdir -p "$OUT"

# Build a well-formed 120-byte profile: magic GM2P, ver 1, reserved 0,
# is_unlocked 0, color 0, sysver 0x40000, spl 0x9A4, then 3x 32B digests.
python3 - "$OUT/good.bin" <<'PY'
import struct, sys
p  = b"GM2P" + struct.pack("<HHIIII", 1, 0, 0, 0, 0x40000, 0x9A4)
p += bytes(range(32)) + bytes(range(32,64)) + bytes(range(64,96))
assert len(p) == 120, len(p)
open(sys.argv[1], "wb").write(p)
PY

# good -> status=0
"$H" profile-parse "$OUT/good.bin" | grep -q 'status=0' \
  || { echo "FAIL: well-formed profile rejected"; exit 1; }

# bad magic -> non-zero
python3 - "$OUT/badmagic.bin" "$OUT/good.bin" <<'PY'
import sys
b = bytearray(open(sys.argv[2],"rb").read()); b[0]=ord('X')
open(sys.argv[1],"wb").write(b)
PY
"$H" profile-parse "$OUT/badmagic.bin" | grep -q 'status=0' \
  && { echo "FAIL: bad magic accepted"; exit 1; } || true

# wrong size -> non-zero
head -c 119 "$OUT/good.bin" > "$OUT/short.bin"
"$H" profile-parse "$OUT/short.bin" | grep -q 'status=0' \
  && { echo "FAIL: short profile accepted"; exit 1; } || true

# color out of range (color=9 at offset 12) -> non-zero
python3 - "$OUT/badcolor.bin" "$OUT/good.bin" <<'PY'
import sys, struct
b = bytearray(open(sys.argv[2],"rb").read())
b[12:16] = struct.pack("<I", 9)
open(sys.argv[1],"wb").write(b)
PY
"$H" profile-parse "$OUT/badcolor.bin" | grep -q 'status=0' \
  && { echo "FAIL: bad color accepted"; exit 1; } || true

# bad version (version=2 at offset 4) -> non-zero
python3 - "$OUT/badversion.bin" "$OUT/good.bin" <<'PY'
import sys, struct
b = bytearray(open(sys.argv[2],"rb").read())
b[4:6] = struct.pack("<H", 2)
open(sys.argv[1],"wb").write(b)
PY
"$H" profile-parse "$OUT/badversion.bin" | grep -q 'status=0' \
  && { echo "FAIL: bad version accepted"; exit 1; } || true

# non-zero reserved (reserved=1 at offset 6) -> non-zero
python3 - "$OUT/badreserved.bin" "$OUT/good.bin" <<'PY'
import sys, struct
b = bytearray(open(sys.argv[2],"rb").read())
b[6:8] = struct.pack("<H", 1)
open(sys.argv[1],"wb").write(b)
PY
"$H" profile-parse "$OUT/badreserved.bin" | grep -q 'status=0' \
  && { echo "FAIL: non-zero reserved accepted"; exit 1; } || true

# is_unlocked out of range (is_unlocked=5 at offset 8) -> non-zero
python3 - "$OUT/badunlocked.bin" "$OUT/good.bin" <<'PY'
import sys, struct
b = bytearray(open(sys.argv[2],"rb").read())
b[8:12] = struct.pack("<I", 5)
open(sys.argv[1],"wb").write(b)
PY
"$H" profile-parse "$OUT/badunlocked.bin" | grep -q 'status=0' \
  && { echo "FAIL: bad is_unlocked accepted"; exit 1; } || true

echo "PASS: 076 mode2 profile parse"
