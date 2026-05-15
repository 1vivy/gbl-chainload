#!/usr/bin/env bash
# 056_mode2_profile_lint.sh — host lint/synthetic test for mode-2 profile flow.
set -euo pipefail
cd "$(dirname "$0")/.."

fail=0
for path in \
  scripts/generate-mode2-profile.py \
  scripts/make-mode2-profile-zip.sh \
  zip/mode2-profile/META-INF/com/google/android/update-binary; do
  [[ -f "$path" ]] || { echo "FAIL: missing $path" >&2; fail=1; }
  [[ -x "$path" ]] || { echo "FAIL: $path must be executable" >&2; fail=1; }
done

grep -q '/sdcard/gbl-chainload_profile.xml' zip/mode2-profile/META-INF/com/google/android/update-binary || { echo "FAIL: installer must use profile convention" >&2; fail=1; }
grep -q '/sdcard/stock_vbmeta.img' zip/mode2-profile/META-INF/com/google/android/update-binary || { echo "FAIL: installer must use stock vbmeta convention" >&2; fail=1; }
grep -q '/sdcard/backup_abl.img' zip/mode2-profile/META-INF/com/google/android/update-binary || { echo "FAIL: mode-2 ZIP must depend on cache-ABL backup convention" >&2; fail=1; }
grep -q 'profile_included' zip/mode2-profile/META-INF/com/google/android/update-binary || { echo "FAIL: installer must honor included profiles" >&2; fail=1; }
grep -q 'payload/gbl-chainload_profile.xml' zip/mode2-profile/META-INF/com/google/android/update-binary || { echo "FAIL: installer must extract included profile payloads" >&2; fail=1; }

python3 -m py_compile scripts/generate-mode2-profile.py

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
printf 'AVB0synthetic-vbmeta' > "$TMP/stock_vbmeta.img"
scripts/generate-mode2-profile.py --stock-vbmeta "$TMP/stock_vbmeta.img" --out "$TMP/profile.xml" >/dev/null
grep -q '<gbl-chainload-profile' "$TMP/profile.xml" || { echo "FAIL: profile XML missing root" >&2; fail=1; }
grep -q '<sha256>' "$TMP/profile.xml" || { echo "FAIL: profile XML missing sha256" >&2; fail=1; }

scripts/make-mode2-profile-zip.sh --profile "$TMP/profile.xml" --out "$TMP/mode2-profile.zip" >/dev/null
python3 - "$TMP/mode2-profile.zip" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    names = set(z.namelist())
    assert "payload/manifest" in names
    assert "payload/gbl-chainload_profile.xml" in names
    manifest = z.read("payload/manifest").decode()
    assert "profile_included=1" in manifest
print("ok synthetic mode2 profile zip")
PY

[[ $fail -eq 0 ]] && echo "ok 056_mode2_profile_lint" || exit 1
