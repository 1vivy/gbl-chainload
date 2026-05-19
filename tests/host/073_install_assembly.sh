#!/usr/bin/env bash
# tests/host/073_install_assembly.sh — assemble the three mode-N-install ZIPs,
# verify each carries the shared install-common.sh + its own mode files + its
# base EFI and nothing from another mode, and lint the staged mode scripts.
set -euo pipefail
cd "$(dirname "$0")/../.."

OUT=tests/host/.last/073
rm -rf "$OUT"; mkdir -p "$OUT"

for n in 0 1 2; do
  MODE="mode-$n-install"
  bash scripts/build-recovery-zip.sh --mode "$MODE" >/dev/null
  ZIP="dist/gbl-chainload-$MODE.zip"
  [ -f "$ZIP" ] || { echo "FAIL: $ZIP not produced"; exit 1; }

  for e in META-INF/com/google/android/update-binary \
           META-INF/com/google/android/updater-script \
           core/ui.sh core/env.sh core/ota.sh core/busybox.sh \
           core/partition.sh core/safety.sh core/install_abl.sh \
           modes/SELECTED modes/install-common.sh \
           "modes/$MODE.conf" "modes/$MODE.sh" \
           bin/fv-unwrap bin/abl-patcher bin/gbl-pack bin/gbl-commit \
           bin/busybox-arm64 \
           "base/mode-$n.efi" SHA256SUMS; do
    unzip -l "$ZIP" | grep -q "[ /]$e\$" \
      || { echo "FAIL: $ZIP missing $e"; exit 1; }
  done

  unzip -p "$ZIP" modes/SELECTED | grep -qx "$MODE" \
    || { echo "FAIL: $ZIP SELECTED is not '$MODE'"; exit 1; }
  # No diag/graft mode files leak in (install-common.sh is shared and stays).
  if unzip -l "$ZIP" | grep -qE 'modes/(diag|graft)\.'; then
    echo "FAIL: $ZIP carries diag/graft mode files"; exit 1
  fi
  # No other install mode's .sh/.conf or base EFI.
  for o in 0 1 2; do
    [ "$o" = "$n" ] && continue
    if unzip -l "$ZIP" | grep -qE "(modes/mode-$o-install\.|base/mode-$o\.efi)"; then
      echo "FAIL: $ZIP carries mode-$o files"; exit 1
    fi
  done

  unzip -o "$ZIP" -d "$OUT/$MODE" >/dev/null
  ( cd "$OUT/$MODE" && sha256sum -c --status SHA256SUMS ) \
    || { echo "FAIL: $ZIP SHA256SUMS mismatch"; exit 1; }
  shellcheck -s sh "$OUT/$MODE/modes/$MODE.sh" \
                   "$OUT/$MODE/modes/install-common.sh" \
    || { echo "FAIL: staged $MODE scripts fail shellcheck"; exit 1; }
done

echo "PASS: 073 install assembly"
