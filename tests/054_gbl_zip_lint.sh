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
grep -q '/sdcard/backup_abl.img' zip/gbl-chainload/META-INF/com/google/android/update-binary \
  || { echo "FAIL: installer must verify /sdcard/backup_abl.img" >&2; fail=1; }
grep -q 'sha256sum' zip/gbl-chainload/META-INF/com/google/android/update-binary \
  || { echo "FAIL: installer must verify hashes" >&2; fail=1; }
grep -q 'efisp' zip/gbl-chainload/META-INF/com/google/android/update-binary \
  || { echo "FAIL: installer must explicitly target EFISP" >&2; fail=1; }

[[ -x scripts/make-gbl-chainload-zip.sh ]] \
  || { echo "FAIL: packager must be executable" >&2; fail=1; }
[[ -x zip/gbl-chainload/META-INF/com/google/android/update-binary ]] \
  || { echo "FAIL: update-binary must be executable" >&2; fail=1; }

[[ $fail -eq 0 ]] && echo "ok 054_gbl_zip_lint" || exit 1
