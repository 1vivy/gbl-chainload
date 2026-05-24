#!/usr/bin/env bash
# tests/host/094_gbl_pack_manifest.sh — gbl-pack --manifest <bits> emits a
# GBLP1_TYPE_MANIFEST entry the Task 1 parser can locate (engine-rework PR1
# / Task 3). Exercises all 3 acceptance criteria:
#   1. --manifest 0x01 -> container with present=1, cap_bits=0x0001.
#   2. no --manifest   -> container with present=0 (no manifest entry).
#   3. --manifest 0x04 -> rejected with the documented error string + rc 2.
set -euo pipefail
cd "$(dirname "$0")/../.."

# Reproducible gbl-pack output (see 060_pack_roundtrip.sh).
: "${SOURCE_DATE_EPOCH:=0}"
export SOURCE_DATE_EPOCH

cargo build --release --quiet -p gbl
PATH="$PWD/target/release:$PATH"; export PATH
make -s -C tests/host/helpers parser_harness

GP=(gbl pack)
H=tests/host/helpers/parser_harness
OUT=tests/host/.last/094
mkdir -p "$OUT"

# A minimal valid 120-byte mode2_profile so we have something to pack with.
python3 - "$OUT/profile.bin" <<'PY'
import struct, sys
b  = b"GM2P" + struct.pack("<HHIIII", 1, 0, 0, 0, 0x40000, 0x9A4)
b += bytes(96)
assert len(b) == 120, len(b)
open(sys.argv[1], "wb").write(b)
PY

# --- AC1: --manifest 0x01 emits a manifest entry the parser sees. -------
"${GP[@]}" --mode2-profile "$OUT/profile.bin" --manifest 0x01 \
      --out "$OUT/with-manifest.bin" 2>"$OUT/with.log" \
  || { echo "FAIL: gbl-pack --manifest 0x01 returned non-zero"; cat "$OUT/with.log"; exit 1; }

"$H" find-manifest "$OUT/with-manifest.bin" >"$OUT/with-find.log" 2>&1 \
  || { echo "FAIL: parser_harness find-manifest returned non-zero"; cat "$OUT/with-find.log"; exit 1; }
grep -q 'status=0' "$OUT/with-find.log" \
  || { echo "FAIL: manifest find status != 0"; cat "$OUT/with-find.log"; exit 1; }
grep -q 'present=1' "$OUT/with-find.log" \
  || { echo "FAIL: manifest present != 1"; cat "$OUT/with-find.log"; exit 1; }
grep -q 'bits=0x0001' "$OUT/with-find.log" \
  || { echo "FAIL: manifest cap_bits != 0x0001"; cat "$OUT/with-find.log"; exit 1; }

# Decimal form of --manifest is equally accepted (cap_bits=0x0002).
"${GP[@]}" --mode2-profile "$OUT/profile.bin" --manifest 2 \
      --out "$OUT/dec-manifest.bin" 2>"$OUT/dec.log" \
  || { echo "FAIL: gbl-pack --manifest 2 (decimal) returned non-zero"; cat "$OUT/dec.log"; exit 1; }
"$H" find-manifest "$OUT/dec-manifest.bin" >"$OUT/dec-find.log" 2>&1 \
  || { echo "FAIL: parser_harness find-manifest (decimal) returned non-zero"; cat "$OUT/dec-find.log"; exit 1; }
grep -q 'bits=0x0002' "$OUT/dec-find.log" \
  || { echo "FAIL: manifest cap_bits != 0x0002 for decimal '2'"; cat "$OUT/dec-find.log"; exit 1; }

# --- AC2: absence — no --manifest flag => present=0 (no entry). ---------
"${GP[@]}" --mode2-profile "$OUT/profile.bin" \
      --out "$OUT/no-manifest.bin" 2>"$OUT/no.log" \
  || { echo "FAIL: gbl-pack (no --manifest) returned non-zero"; cat "$OUT/no.log"; exit 1; }

"$H" find-manifest "$OUT/no-manifest.bin" >"$OUT/no-find.log" 2>&1 \
  || { echo "FAIL: parser_harness find-manifest returned non-zero (absence case)"; cat "$OUT/no-find.log"; exit 1; }
grep -q 'status=0' "$OUT/no-find.log" \
  || { echo "FAIL: manifest find status != 0 (absence)"; cat "$OUT/no-find.log"; exit 1; }
grep -q 'present=0' "$OUT/no-find.log" \
  || { echo "FAIL: manifest present != 0 (absence)"; cat "$OUT/no-find.log"; exit 1; }

# --- AC3: reserved-bit rejection at packing time. ------------------------
set +e
"${GP[@]}" --mode2-profile "$OUT/profile.bin" --manifest 0x04 \
      --out "$OUT/bad.bin" 2>"$OUT/bad.log"
rc=$?
set -e
[ "$rc" = "2" ] \
  || { echo "FAIL: --manifest 0x04 exit code $rc != 2"; cat "$OUT/bad.log"; exit 1; }
grep -q 'bad --manifest bits (reserved bits set)' "$OUT/bad.log" \
  || { echo "FAIL: expected error string missing"; cat "$OUT/bad.log"; exit 1; }

echo "PASS: 094 gbl-pack manifest"
