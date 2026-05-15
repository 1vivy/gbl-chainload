#!/usr/bin/env bash
# 054_gbl_zip_lint.sh — host lint for gbl-chainload recovery ZIP packaging.
set -euo pipefail
cd "$(dirname "$0")/.."

fail=0
for path in \
  scripts/make-gbl-chainload-zip.sh \
  zip/gbl-chainload/META-INF/com/google/android/update-binary \
  zip/gbl-chainload/META-INF/com/google/android/updater-script; do
  [[ -f "$path" ]] || { echo "FAIL: missing $path" >&2; fail=1; }
done

grep -q '/sdcard/backup_abl.img' scripts/make-gbl-chainload-zip.sh \
  || { echo "FAIL: packager must document /sdcard/backup_abl.img" >&2; fail=1; }
grep -q 'whole, known-good' scripts/make-gbl-chainload-zip.sh \
  || { echo "FAIL: packager must document backup ABL quality requirement" >&2; fail=1; }
grep -q '/sdcard/backup_abl.img' zip/gbl-chainload/META-INF/com/google/android/update-binary \
  || { echo "FAIL: installer must require /sdcard/backup_abl.img" >&2; fail=1; }
grep -q 'sha256sum' zip/gbl-chainload/META-INF/com/google/android/update-binary \
  || { echo "FAIL: installer must verify EFI payload hash" >&2; fail=1; }
! grep -q -- '--ota-abl\|--backup-abl\|cached_ota_abl_sha256\|backup_abl_sha256\|ota_abl_expected_path' scripts/make-gbl-chainload-zip.sh \
  || { echo "FAIL: ZIP builder must only require --efi plus --out" >&2; fail=1; }
grep -q 'androidboot.slot_suffix' zip/gbl-chainload/META-INF/com/google/android/update-binary \
  || { echo "FAIL: installer must detect current slot" >&2; fail=1; }
grep -q 'Selected inactive-slot OTA ABL target' zip/gbl-chainload/META-INF/com/google/android/update-binary \
  || { echo "FAIL: installer must target inactive OTA ABL slot" >&2; fail=1; }
grep -q 'WARNING: restoring GBL-capable fallback ABL' zip/gbl-chainload/META-INF/com/google/android/update-binary \
  || { echo "FAIL: installer must loudly announce ABL restore" >&2; fail=1; }
grep -q '/sdcard/gbl-chainload/ota_abl' zip/gbl-chainload/META-INF/com/google/android/update-binary \
  || { echo "FAIL: installer must back up OTA ABL before restore" >&2; fail=1; }
grep -q 'dd if="$BACKUP_ABL" of="$TARGET_ABL"' zip/gbl-chainload/META-INF/com/google/android/update-binary \
  || { echo "FAIL: installer must restore backup ABL to selected target" >&2; fail=1; }
grep -q 'read-back verification' zip/gbl-chainload/META-INF/com/google/android/update-binary \
  || { echo "FAIL: installer must read-back verify ABL restore" >&2; fail=1; }
grep -q 'efisp' zip/gbl-chainload/META-INF/com/google/android/update-binary \
  || { echo "FAIL: installer must explicitly target EFISP" >&2; fail=1; }
grep -q 'Artifact log:' zip/gbl-chainload/META-INF/com/google/android/update-binary \
  || { echo "FAIL: installer must report an artifact log path" >&2; fail=1; }
grep -q 'too small to be a whole ABL image' zip/gbl-chainload/META-INF/com/google/android/update-binary \
  || { echo "FAIL: installer must reject obviously truncated backup ABLs" >&2; fail=1; }
grep -q '\[.*\$STEP_TOTAL\]' zip/gbl-chainload/META-INF/com/google/android/update-binary \
  || { echo "FAIL: installer must show numbered progress steps" >&2; fail=1; }
grep -q 'EFISP read-back verification passed' zip/gbl-chainload/META-INF/com/google/android/update-binary \
  || { echo "FAIL: installer must read-back verify EFISP payload" >&2; fail=1; }
[[ -x scripts/make-gbl-chainload-zip.sh ]] \
  || { echo "FAIL: packager must be executable" >&2; fail=1; }
[[ -x zip/gbl-chainload/META-INF/com/google/android/update-binary ]] \
  || { echo "FAIL: update-binary must be executable" >&2; fail=1; }

[[ $fail -eq 0 ]] && echo "ok 054_gbl_zip_lint" || exit 1
