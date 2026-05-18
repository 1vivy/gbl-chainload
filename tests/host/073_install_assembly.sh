#!/usr/bin/env bash
# tests/host/073_install_assembly.sh — assemble gbl-chainload-install.zip,
# verify its contents, and lint the staged install.sh.
set -euo pipefail
cd "$(dirname "$0")/../.."

OUT=tests/host/.last/073
rm -rf "$OUT"; mkdir -p "$OUT"

bash scripts/build-recovery-zip.sh --mode install >/dev/null
ZIP=dist/gbl-chainload-install.zip
[ -f "$ZIP" ] || { echo "FAIL: $ZIP not produced"; exit 1; }

for e in META-INF/com/google/android/update-binary \
         META-INF/com/google/android/updater-script \
         core/ui.sh core/env.sh core/ota.sh core/busybox.sh core/partition.sh core/safety.sh \
         modes/SELECTED modes/install.conf modes/install.sh \
         bin/fv-unwrap bin/abl-patcher bin/gbl-pack bin/gbl-commit bin/busybox-arm64 \
         base/mode-1.efi SHA256SUMS; do
  unzip -l "$ZIP" | grep -q "[ /]$e\$" || { echo "FAIL: $ZIP missing $e"; exit 1; }
done

unzip -p "$ZIP" modes/SELECTED | grep -qx install \
  || { echo "FAIL: SELECTED is not 'install'"; exit 1; }
if unzip -l "$ZIP" | grep -qE 'modes/(diag|graft|profile)'; then
  echo "FAIL: non-selected modes were not pruned"; exit 1
fi

unzip -o "$ZIP" -d "$OUT/x" >/dev/null
( cd "$OUT/x" && sha256sum -c --status SHA256SUMS ) \
  || { echo "FAIL: SHA256SUMS mismatch"; exit 1; }
shellcheck -s sh "$OUT/x/modes/install.sh" \
  || { echo "FAIL: staged install.sh fails shellcheck"; exit 1; }

echo "PASS: 073 install assembly"
