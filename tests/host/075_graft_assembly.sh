#!/usr/bin/env bash
# tests/host/075_graft_assembly.sh — assemble gbl-chainload-graft.zip.
set -euo pipefail
cd "$(dirname "$0")/../.."

OUT=tests/host/.last/075
rm -rf "$OUT"; mkdir -p "$OUT"

bash scripts/build-recovery-zip.sh --mode graft >/dev/null
ZIP=dist/gbl-chainload-graft.zip
[ -f "$ZIP" ] || { echo "FAIL: $ZIP not produced"; exit 1; }

for e in META-INF/com/google/android/update-binary \
         core/ui.sh core/env.sh core/ota.sh core/busybox.sh core/partition.sh core/safety.sh \
         modes/SELECTED modes/graft.conf modes/graft.sh \
         bin/vbmeta-graft bin/gbl-commit bin/busybox-arm64 SHA256SUMS; do
  unzip -l "$ZIP" | grep -q "[ /]$e\$" || { echo "FAIL: $ZIP missing $e"; exit 1; }
done

unzip -p "$ZIP" modes/SELECTED | grep -qx graft \
  || { echo "FAIL: SELECTED is not 'graft'"; exit 1; }
if unzip -l "$ZIP" | grep -qE 'modes/(diag|install|profile)'; then
  echo "FAIL: non-selected modes were not pruned"; exit 1
fi

unzip -o "$ZIP" -d "$OUT/x" >/dev/null
( cd "$OUT/x" && sha256sum -c --status SHA256SUMS ) \
  || { echo "FAIL: SHA256SUMS mismatch"; exit 1; }
shellcheck -s sh "$OUT/x/modes/graft.sh" \
  || { echo "FAIL: staged graft.sh fails shellcheck"; exit 1; }

echo "PASS: 075 graft assembly"
