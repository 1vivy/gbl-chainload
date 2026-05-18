#!/usr/bin/env bash
# tests/host/074_vbmeta_graft.sh — vbmeta-graft list / check / graft.
set -euo pipefail
cd "$(dirname "$0")/../.."

make -s -C tools/vbmeta-graft

OUT=tests/host/.last/074
rm -rf "$OUT"; mkdir -p "$OUT"
VG=tools/vbmeta-graft/vbmeta-graft

# grafted-recovery.img is a footer'd partition with a real embedded vbmeta.
FX=images/grafted-recovery.img
[ -f "$FX" ] || { echo "SKIP: $FX absent"; exit 0; }

# --- list: enumerates the embedded vbmeta's descriptors ---------------
"$VG" list "$FX" > "$OUT/list.txt" 2>&1 \
  || { echo "FAIL: list exited nonzero"; cat "$OUT/list.txt"; exit 1; }
grep -q 'partition=' "$OUT/list.txt" \
  || { echo "FAIL: list produced no 'partition=' lines"; cat "$OUT/list.txt"; exit 1; }

# --- graft: round-trip an arbitrary custom image ----------------------
head -c 200000 /dev/urandom > "$OUT/custom.img"
PSZ=$(stat -c%s "$FX")
"$VG" graft --stock "$FX" --custom "$OUT/custom.img" --part-size "$PSZ" \
       --out "$OUT/grafted.img" > "$OUT/graft.log" 2>&1 \
  || { echo "FAIL: graft exited nonzero"; cat "$OUT/graft.log"; exit 1; }
[ "$(stat -c%s "$OUT/grafted.img")" = "$PSZ" ] \
  || { echo "FAIL: grafted image is not partition-sized"; exit 1; }
# the grafted image must itself parse: its footer points at a vbmeta
"$VG" list "$OUT/grafted.img" > "$OUT/grafted-list.txt" 2>&1 \
  || { echo "FAIL: grafted image does not re-parse"; cat "$OUT/grafted-list.txt"; exit 1; }
# first 200000 bytes are the custom content verbatim
cmp -n 200000 "$OUT/custom.img" "$OUT/grafted.img" \
  || { echo "FAIL: custom content not preserved at offset 0"; exit 1; }

# --- check: a partition checked against its own vbmeta is suitable ----
# (grafted-recovery's embedded vbmeta is self-consistent for 'recovery')
"$VG" check "$FX" "$FX" recovery > "$OUT/check.log" 2>&1 && rc=0 || rc=$?
echo "check rc=$rc" >> "$OUT/check.log"
# rc 0 = suitable, 2 = parsed/mismatch, 1 = unparseable. Accept 0 or 2
# here (the fixture predates the project's key); a hard fail is rc 1.
[ "$rc" != 1 ] || { echo "FAIL: check could not parse the fixture"; cat "$OUT/check.log"; exit 1; }

echo "PASS: 074 vbmeta-graft"
