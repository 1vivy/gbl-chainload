#!/usr/bin/env bash
# Build the mode-2 profile recovery ZIP skeleton.
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage: scripts/make-mode2-profile-zip.sh --out dist/mode2-profile.zip [--profile profile.xml]

If no profile is included, the recovery installer creates
/sdcard/gbl-chainload_profile.xml from /sdcard/stock_vbmeta.img.
EOF
  exit 2
}

OUT=""
PROFILE=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --out) OUT="$2"; shift 2 ;;
    --profile) PROFILE="$2"; shift 2 ;;
    -h|--help) usage ;;
    *) echo "unknown arg: $1" >&2; usage ;;
  esac
done
[[ -n "$OUT" ]] || usage
if [[ -n "$PROFILE" && ! -f "$PROFILE" ]]; then
  echo "missing profile: $PROFILE" >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$ROOT/Build/zip/mode2-profile"
rm -rf "$WORK"
mkdir -p "$WORK/payload" "$WORK/META-INF/com/google/android" "$(dirname "$OUT")"
cp "$ROOT/zip/mode2-profile/META-INF/com/google/android/update-binary" "$WORK/META-INF/com/google/android/update-binary"
cp "$ROOT/zip/mode2-profile/META-INF/com/google/android/updater-script" "$WORK/META-INF/com/google/android/updater-script"
chmod +x "$WORK/META-INF/com/google/android/update-binary"
if [[ -n "$PROFILE" ]]; then
  cp "$PROFILE" "$WORK/payload/gbl-chainload_profile.xml"
fi

cat > "$WORK/payload/manifest" <<EOF
type=mode2-profile
profile_included=$([[ -n "$PROFILE" ]] && echo 1 || echo 0)
profile_expected_path=/sdcard/gbl-chainload_profile.xml
stock_vbmeta_expected_path=/sdcard/stock_vbmeta.img
backup_abl_expected_path=/sdcard/backup_abl.img
EOF

python3 - "$WORK" "$OUT" <<'PY'
import os, sys, zipfile
root, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED) as z:
    for dirpath, _, filenames in os.walk(root):
        for name in filenames:
            path = os.path.join(dirpath, name)
            z.write(path, os.path.relpath(path, root))
PY
echo "wrote $OUT"
