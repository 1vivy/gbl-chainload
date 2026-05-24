#!/usr/bin/env bash
# tests/host/091_vbmeta_graft_py.sh — vbmeta-graft.py wrapper host test.
set -euo pipefail
cd "$(dirname "$0")/../.."

cargo build --release --quiet -p gbl

OUT=tests/host/.last/091
rm -rf "$OUT"; mkdir -p "$OUT"

FX=tests/images/grafted-recovery.img
[ -f "$FX" ] || { echo "SKIP: $FX absent"; exit 0; }

# PR2 Task 8: scripts/vbmeta-graft.py now resolves `gbl` (former
# `vbmeta-graft`) via --bin-dir / --tools-dir, then dispatches the
# `avb graft` / `avb check` subcommands.
WRAP=scripts/vbmeta-graft.py
TMPTOOLS="$OUT/tools"
mkdir -p "$TMPTOOLS"
cp "$PWD/target/release/gbl" "$TMPTOOLS/"

PSZ=$(stat -c%s "$FX")
SMALL="$OUT/custom-small.img"
head -c 200000 /dev/urandom > "$SMALL"

# 1) Footered custom image: default part-size comes from custom file size.
"$WRAP" --stock "$FX" --custom "$FX" --out "$OUT/default.img" \
  --bin-dir "$TMPTOOLS" > "$OUT/default.log" 2>&1 \
  || { echo "FAIL: default-size graft exited nonzero"; cat "$OUT/default.log"; exit 1; }
[ "$(stat -c%s "$OUT/default.img")" = "$PSZ" ] \
  || { echo "FAIL: default-size output not fixture-sized"; exit 1; }
"$TMPTOOLS/gbl" avb list "$OUT/default.img" > "$OUT/default-list.txt" 2>&1 \
  || { echo "FAIL: default-size output does not parse"; cat "$OUT/default-list.txt"; exit 1; }

# 2) Bare custom small image: --size-from should drive part-size.
"$WRAP" --stock "$FX" --custom "$SMALL" --size-from "$FX" --out "$OUT/size-from.img" \
  --tools-dir "$TMPTOOLS" > "$OUT/size-from.log" 2>&1 \
  || { echo "FAIL: size-from graft exited nonzero"; cat "$OUT/size-from.log"; exit 1; }
[ "$(stat -c%s "$OUT/size-from.img")" = "$PSZ" ] \
  || { echo "FAIL: size-from output not fixture-sized"; exit 1; }
"$TMPTOOLS/gbl" avb list "$OUT/size-from.img" > "$OUT/size-from-list.txt" 2>&1 \
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

# Schema check: vbmeta-graft.py default-list emits the recovery partition
# tagged graftable. Replaces the prior byte-identity golden.
grep -qE '^partition=recovery type=hash graftable=yes$' "$OUT/default-list.txt" \
  || { echo "FAIL 091: default-list.txt missing 'partition=recovery type=hash graftable=yes'"; cat "$OUT/default-list.txt"; exit 1; }
grep -qE '^descriptor type=other$' "$OUT/default-list.txt" \
  || { echo "FAIL 091: default-list.txt missing 'descriptor type=other'"; cat "$OUT/default-list.txt"; exit 1; }

echo "PASS: 091 vbmeta-graft.py"
