#!/usr/bin/env bash
# tests/host/091_vbmeta_graft_py.sh — vbmeta-graft.py wrapper host test.
set -euo pipefail
cd "$(dirname "$0")/../.."

make -s -C tools/vbmeta-graft

OUT=tests/host/.last/091
rm -rf "$OUT"; mkdir -p "$OUT"

FX=tests/images/grafted-recovery.img
[ -f "$FX" ] || { echo "SKIP: $FX absent"; exit 0; }

VG=tools/vbmeta-graft/vbmeta-graft
WRAP=scripts/vbmeta-graft.py
TMPTOOLS="$OUT/tools"
mkdir -p "$TMPTOOLS"
cp "$VG" "$TMPTOOLS/"

PSZ=$(stat -c%s "$FX")
SMALL="$OUT/custom-small.img"
head -c 200000 /dev/urandom > "$SMALL"

# 1) Footered custom image: default part-size comes from custom file size.
"$WRAP" --stock "$FX" --custom "$FX" --out "$OUT/default.img" \
  --bin-dir "$TMPTOOLS" > "$OUT/default.log" 2>&1 \
  || { echo "FAIL: default-size graft exited nonzero"; cat "$OUT/default.log"; exit 1; }
[ "$(stat -c%s "$OUT/default.img")" = "$PSZ" ] \
  || { echo "FAIL: default-size output not fixture-sized"; exit 1; }
"$TMPTOOLS/vbmeta-graft" list "$OUT/default.img" > "$OUT/default-list.txt" 2>&1 \
  || { echo "FAIL: default-size output does not parse"; cat "$OUT/default-list.txt"; exit 1; }

# 2) Bare custom small image: --size-from should drive part-size.
"$WRAP" --stock "$FX" --custom "$SMALL" --size-from "$FX" --out "$OUT/size-from.img" \
  --tools-dir "$TMPTOOLS" > "$OUT/size-from.log" 2>&1 \
  || { echo "FAIL: size-from graft exited nonzero"; cat "$OUT/size-from.log"; exit 1; }
[ "$(stat -c%s "$OUT/size-from.img")" = "$PSZ" ] \
  || { echo "FAIL: size-from output not fixture-sized"; exit 1; }
"$TMPTOOLS/vbmeta-graft" list "$OUT/size-from.img" > "$OUT/size-from-list.txt" 2>&1 \
  || { echo "FAIL: size-from output does not parse"; cat "$OUT/size-from-list.txt"; exit 1; }
cmp -n 200000 "$SMALL" "$OUT/size-from.img" \
  || { echo "FAIL: bare custom content not preserved at offset 0"; exit 1; }

# 3) Dry-run reports the resolved part-size and creates no output.
DRYOUT="$OUT/dry-run.img"
"$WRAP" --stock "$FX" --custom "$FX" --out "$DRYOUT" --dry-run \
  --bin-dir "$TMPTOOLS" > "$OUT/dry-run.log" 2>&1 \
  || { echo "FAIL: dry-run exited nonzero"; cat "$OUT/dry-run.log"; exit 1; }
grep -q "part-size=$PSZ (custom image size)" "$OUT/dry-run.log" \
  || { echo "FAIL: dry-run did not report custom-image part-size"; cat "$OUT/dry-run.log"; exit 1; }
[ ! -e "$DRYOUT" ] || { echo "FAIL: dry-run unexpectedly created output"; exit 1; }

# Golden parity assertion (frozen C-tool output). $SMALL uses /dev/urandom
# so size-from.img / size-from-list.txt are excluded.  default.img grafts
# the fixture onto itself and IS deterministic, but ~100 MB — too heavy for
# an in-tree golden; tracked but not asserted here.
diff -u tests/host/goldens/091/default-list.txt "$OUT/default-list.txt" \
  || { echo "FAIL 091 golden: default-list.txt diverged from frozen C-tool output"; exit 1; }

echo "PASS: 091 vbmeta-graft.py"
