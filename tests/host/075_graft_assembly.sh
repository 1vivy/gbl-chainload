#!/usr/bin/env bash
# tests/host/075_graft_assembly.sh — assemble gbl-chainload-graft.zip.
set -euo pipefail
cd "$(dirname "$0")/../.."

OUT=tests/host/.last/075
rm -rf "$OUT"; mkdir -p "$OUT"

bash scripts/build-recovery-zip.sh --mode graft >/dev/null
ZIP=dist/gbl-chainload-graft.zip
[ -f "$ZIP" ] || { echo "FAIL: $ZIP not produced"; exit 1; }
unzip -l "$ZIP" > "$OUT/graft.list"

for e in META-INF/com/google/android/update-binary \
         core/ui.sh core/env.sh core/ota.sh core/busybox.sh core/partition.sh core/safety.sh \
         modes/SELECTED modes/graft.conf modes/graft.sh modes/graft-common.sh \
         bin/vbmeta-graft bin/gbl-commit bin/busybox-arm64 SHA256SUMS; do
  grep -q "[ /]$e\$" "$OUT/graft.list" || { echo "FAIL: $ZIP missing $e"; exit 1; }
done

unzip -p "$ZIP" modes/SELECTED | grep -qx graft \
  || { echo "FAIL: SELECTED is not 'graft'"; exit 1; }
if grep -qE 'modes/(diag|mode-[012]-install)\.' "$OUT/graft.list"; then
  echo "FAIL: non-selected modes were not pruned"; exit 1
fi

unzip -o "$ZIP" -d "$OUT/x" >/dev/null
( cd "$OUT/x" && sha256sum -c --status SHA256SUMS ) \
  || { echo "FAIL: SHA256SUMS mismatch"; exit 1; }
if grep -R -qE 'timeout |/sdcard/gbl_|/sdcard/stock_recovery\.img|/sdcard/efisp\.bak' \
     "$OUT/x/core" "$OUT/x/modes" "$OUT/x/META-INF"; then
  echo "FAIL: graft ZIP contains timeout prompts or legacy sdcard paths"; exit 1
fi
grep -R -q 'GBL_STATE_DIR/graft-candidate' "$OUT/x/modes" \
  || { echo "FAIL: graft ZIP missing graft-candidate path"; exit 1; }
grep -R -q 'GBL_STATE_DIR/graft-target' "$OUT/x/modes" \
  || { echo "FAIL: graft ZIP missing graft-target path"; exit 1; }
shellcheck -s sh "$OUT/x/modes/graft.sh" "$OUT/x/modes/graft-common.sh" \
  || { echo "FAIL: staged graft.sh fails shellcheck"; exit 1; }

echo "PASS: 075 graft assembly"
