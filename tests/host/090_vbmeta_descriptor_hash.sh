#!/usr/bin/env bash
# tests/host/090_vbmeta_descriptor_hash.sh — vbmeta-graft list-hash.
set -euo pipefail
cd "$(dirname "$0")/../.."

make -s -C tools/vbmeta-graft

FX=tests/images/grafted-recovery.img
[ -f "$FX" ] || { echo "SKIP: $FX absent"; exit 0; }

OUT=tests/host/.last/090
rm -rf "$OUT"; mkdir -p "$OUT/byname"
VG=tools/vbmeta-graft/vbmeta-graft

# Build a synthetic by-name dir: copy the fixture into byname/recovery_a.
# The fixture's embedded vbmeta is what list-hash will both parse and (for
# chain descriptors, if any) probe.
cp "$FX" "$OUT/byname/recovery_a"

# Provide an active vbmeta — for this test we use the fixture itself as the
# main vbmeta target. list-hash walks its descriptors against the byname
# dir. The exact descriptor set depends on the fixture; the test asserts
# only on stable shape, not specific partitions.
GBL_VBMETA_SLOT=a "$VG" list-hash "$FX" "$OUT/byname" > "$OUT/lh.txt" 2>&1 \
  || { echo "FAIL: list-hash exited nonzero"; cat "$OUT/lh.txt"; exit 1; }

# Every emitted partition line must carry digest=, graft=, verdict= fields.
awk '/^partition=/ {
  if (!/digest=/ || !/graft=/ || !/verdict=/) {
    print "BAD LINE:", $0; exit 1
  }
}' "$OUT/lh.txt" \
  || { echo "FAIL: a partition line is missing required fields"; cat "$OUT/lh.txt"; exit 1; }

# There must be at least one partition line.
grep -q '^partition=' "$OUT/lh.txt" \
  || { echo "FAIL: no partition lines in list-hash output"; cat "$OUT/lh.txt"; exit 1; }

# Perturb a body byte and re-run; at least one verdict must be mismatch.
cp "$OUT/byname/recovery_a" "$OUT/byname/recovery_a.bak"
python3 -c '
import sys
p=open(sys.argv[1],"r+b"); p.seek(0); p.write(b"\x00\x00\x00\x00"); p.close()
' "$OUT/byname/recovery_a"

GBL_VBMETA_SLOT=a "$VG" list-hash "$FX" "$OUT/byname" > "$OUT/lh-corrupt.txt" 2>&1 \
  || true
grep -q 'verdict=mismatch' "$OUT/lh-corrupt.txt" \
  || { echo "FAIL: perturbed byname did not produce verdict=mismatch"; cat "$OUT/lh-corrupt.txt"; exit 1; }

# Restore.
mv "$OUT/byname/recovery_a.bak" "$OUT/byname/recovery_a"

# Regression: existing list / check / graft must still work — run 074.
bash tests/host/074_vbmeta_graft.sh > "$OUT/074.log" 2>&1 \
  || { echo "FAIL: 074 regressed"; cat "$OUT/074.log"; exit 1; }

echo "PASS: 090 vbmeta-graft list-hash"
