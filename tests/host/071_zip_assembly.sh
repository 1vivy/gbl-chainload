#!/usr/bin/env bash
# tests/host/071_zip_assembly.sh — build-recovery-zip.sh assembly + skew guard.
set -euo pipefail
cd "$(dirname "$0")/../.."

OUT=tests/host/.last/071
rm -rf "$OUT"; mkdir -p "$OUT"

# --- 1. assemble the diag ZIP ---------------------------------------
bash scripts/build-recovery-zip.sh --mode diag >/dev/null
ZIP=dist/gbl-chainload-diag.zip
[ -f "$ZIP" ] || { echo "FAIL: $ZIP not produced"; exit 1; }

for e in META-INF/com/google/android/update-binary \
         META-INF/com/google/android/updater-script \
         core/ui.sh core/env.sh core/ota.sh core/busybox.sh core/partition.sh core/safety.sh \
         modes/SELECTED modes/diag.conf modes/diag.sh SHA256SUMS; do
  unzip -l "$ZIP" | grep -q "[ /]$e\$" \
    || { echo "FAIL: $ZIP missing $e"; exit 1; }
done

# SELECTED names diag; other modes are pruned out
unzip -p "$ZIP" modes/SELECTED | grep -qx diag \
  || { echo "FAIL: SELECTED is not 'diag'"; exit 1; }
if unzip -l "$ZIP" | grep -qE 'modes/(install|graft|profile)'; then
  echo "FAIL: non-selected modes were not pruned"; exit 1
fi

# SHA256SUMS verifies
unzip -o "$ZIP" -d "$OUT/diag" >/dev/null
( cd "$OUT/diag" && sha256sum -c --status SHA256SUMS ) \
  || { echo "FAIL: SHA256SUMS mismatch"; exit 1; }

# --- 2. skew guard fires on a tampered MANIFEST ---------------------
SUB=zip
cp "$SUB/bin/MANIFEST" "$OUT/MANIFEST.orig"
trap 'cp "$OUT/MANIFEST.orig" "$SUB/bin/MANIFEST"' EXIT
# zero the first hash line so the recomputed hash will not match
sed '0,/^[0-9a-f]\{64\}  /s//0000000000000000000000000000000000000000000000000000000000000000  /' \
  "$OUT/MANIFEST.orig" > "$SUB/bin/MANIFEST"
if bash scripts/build-recovery-zip.sh --mode diag >/dev/null 2>&1; then
  echo "FAIL: skew guard did not fire on a tampered MANIFEST"; exit 1
fi
cp "$OUT/MANIFEST.orig" "$SUB/bin/MANIFEST"

echo "PASS: 071 zip assembly + skew guard"
