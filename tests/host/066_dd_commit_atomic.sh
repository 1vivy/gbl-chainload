#!/usr/bin/env bash
# tests/host/066_dd_commit_atomic.sh — gbl-commit backup + verify cycle.
set -euo pipefail
cd "$(dirname "$0")/../.."

make -s -C tools/gbl-commit

OUT=tests/host/.last/066
mkdir -p "$OUT"

dd if=/dev/urandom of="$OUT/src.bin" bs=1024 count=512 2>/dev/null
dd if=/dev/urandom of="$OUT/dst.bin" bs=1024 count=512 2>/dev/null

ORIG_DST_SHA=$(sha256sum "$OUT/dst.bin" | cut -d' ' -f1)

tools/gbl-commit/gbl-commit \
  --src "$OUT/src.bin" \
  --dst "$OUT/dst.bin" \
  --backup "$OUT/dst.bak" \
  --verify

# Backup must equal original dst.
BAK_SHA=$(sha256sum "$OUT/dst.bak" | cut -d' ' -f1)
[ "$BAK_SHA" = "$ORIG_DST_SHA" ] \
  || { echo "FAIL: backup sha mismatch"; exit 1; }

# Dst must equal src.
SRC_SHA=$(sha256sum "$OUT/src.bin" | cut -d' ' -f1)
NEW_DST_SHA=$(sha256sum "$OUT/dst.bin" | cut -d' ' -f1)
[ "$NEW_DST_SHA" = "$SRC_SHA" ] \
  || { echo "FAIL: dst sha mismatch"; exit 1; }

echo "PASS: 066 dd commit atomic"
