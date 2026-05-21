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
  unzip -l "$ZIP" > "$OUT/$MODE.list"

  common_expected=(META-INF/com/google/android/update-binary \
           META-INF/com/google/android/updater-script \
           core/ui.sh core/env.sh core/ota.sh core/busybox.sh \
           core/partition.sh core/safety.sh core/install_abl.sh \
           modes/SELECTED modes/install-common.sh \
           "modes/$MODE.conf" "modes/$MODE.sh" \
           bin/fv-unwrap bin/abl-patcher bin/gbl-pack bin/gbl-commit \
           bin/busybox-arm64 \
            "base/mode-$n.efi" SHA256SUMS)
  if [ "$n" = 1 ]; then
    common_expected+=(modes/graft-common.sh bin/vbmeta-graft)
  fi
  for e in "${common_expected[@]}"; do
    grep -q "[ /]$e\$" "$OUT/$MODE.list" \
      || { echo "FAIL: $ZIP missing $e"; exit 1; }
  done

  unzip -p "$ZIP" modes/SELECTED | grep -qx "$MODE" \
    || { echo "FAIL: $ZIP SELECTED is not '$MODE'"; exit 1; }
  # No diag/graft mode files leak in (install-common.sh is shared and stays).
  if grep -qE 'modes/(diag|graft)\.' "$OUT/$MODE.list"; then
    echo "FAIL: $ZIP carries diag/graft mode files"; exit 1
  fi
  # No other install mode's .sh/.conf or base EFI.
  for o in 0 1 2; do
    [ "$o" = "$n" ] && continue
    if grep -qE "(modes/mode-$o-install\.|base/mode-$o\.efi)" "$OUT/$MODE.list"; then
      echo "FAIL: $ZIP carries mode-$o files"; exit 1
    fi
  done

  unzip -o "$ZIP" -d "$OUT/$MODE" >/dev/null
  ( cd "$OUT/$MODE" && sha256sum -c --status SHA256SUMS ) \
    || { echo "FAIL: $ZIP SHA256SUMS mismatch"; exit 1; }
  if grep -R -qE 'timeout |/sdcard/efisp\.bak|/sdcard/gbl_|/sdcard/stock_recovery\.img' \
       "$OUT/$MODE/core" "$OUT/$MODE/modes" "$OUT/$MODE/META-INF"; then
    echo "FAIL: $ZIP contains timeout prompts or legacy sdcard paths"; exit 1
  fi
  grep -R -q 'latest_abl.img' "$OUT/$MODE/core" "$OUT/$MODE/modes" \
    || { echo "FAIL: $ZIP missing latest ABL cache backup path"; exit 1; }
  if [ "$n" = 2 ]; then
    grep -R -q 'GBL_STATE_DIR/mode-2/stock_vbmeta.img' "$OUT/$MODE/modes" \
      || { echo "FAIL: mode-2 ZIP missing namespaced stock vbmeta path"; exit 1; }
    grep -R -q 'GBL_STATE_DIR/mode-2/profile.toml' "$OUT/$MODE/modes" \
      || { echo "FAIL: mode-2 ZIP missing namespaced profile TOML path"; exit 1; }
  fi
  shell_targets=("$OUT/$MODE/modes/$MODE.sh" "$OUT/$MODE/modes/install-common.sh")
  [ "$n" = 1 ] && shell_targets+=("$OUT/$MODE/modes/graft-common.sh")
  shellcheck -s sh "${shell_targets[@]}" \
    || { echo "FAIL: staged $MODE scripts fail shellcheck"; exit 1; }

  if [ "$n" = 1 ]; then
    grep -R -q 'GBL_STATE_DIR/graft-candidate' "$OUT/$MODE/modes" \
      || { echo "FAIL: mode-1 ZIP missing graft-candidate path"; exit 1; }
    grep -R -q 'GBL_STATE_DIR/graft-target' "$OUT/$MODE/modes" \
      || { echo "FAIL: mode-1 ZIP missing graft-target path"; exit 1; }
    prewrite_line=$(grep -n 'mode_preinstall_write' "$OUT/$MODE/modes/install-common.sh" | tail -1 | cut -d: -f1)
    efisp_line=$(grep -n 'commit_efisp$' "$OUT/$MODE/modes/install-common.sh" | tail -1 | cut -d: -f1)
    [ "$prewrite_line" -lt "$efisp_line" ] \
      || { echo "FAIL: mode-1 graft hook does not run before EFISP write"; exit 1; }
    grep -q 'dd if="$_src" of="$WORKDIR/custom_recovery.img"' "$OUT/$MODE/modes/mode-1-install.sh" \
      || { echo "FAIL: mode-1 OTA recovery source is not copied to a regular file"; exit 1; }
  fi
done

echo "PASS: 073 install assembly"
