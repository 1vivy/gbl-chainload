#!/usr/bin/env bash
# Build a host-generated recovery ZIP for a cache-ABL gbl-chainload EFI.
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage: scripts/make-gbl-chainload-zip.sh \
  --efi dist/mode-1-cache-abl.efi \
  --backup-abl /path/to/backup_abl.img \
  --out dist/gbl-chainload.zip \
  [--efisp-dest gbl-chainload.efi]

The recovery installer verifies /sdcard/backup_abl.img against the supplied
backup ABL hash before writing the EFI payload to EFISP.
EOF
  exit 2
}

EFI=""
BACKUP_ABL=""
OUT=""
EFISP_DEST="gbl-chainload.efi"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --efi) EFI="$2"; shift 2 ;;
    --backup-abl) BACKUP_ABL="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --efisp-dest) EFISP_DEST="$2"; shift 2 ;;
    -h|--help) usage ;;
    *) echo "unknown arg: $1" >&2; usage ;;
  esac
done

[[ -n "$EFI" && -n "$BACKUP_ABL" && -n "$OUT" ]] || usage
[[ -f "$EFI" ]] || { echo "missing EFI: $EFI" >&2; exit 2; }
[[ -f "$BACKUP_ABL" ]] || { echo "missing backup ABL: $BACKUP_ABL" >&2; exit 2; }

EFI_SIZE=$(stat -c%s "$EFI")
[[ "$EFI_SIZE" -le 3145728 ]] || { echo "EFI exceeds 3MiB EFISP size assumption: $EFI_SIZE" >&2; exit 2; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$ROOT/Build/zip/gbl-chainload"
rm -rf "$WORK"
mkdir -p "$WORK/payload" "$WORK/META-INF/com/google/android" "$(dirname "$OUT")"

cp "$ROOT/zip/gbl-chainload/META-INF/com/google/android/update-binary" \
  "$WORK/META-INF/com/google/android/update-binary"
cp "$ROOT/zip/gbl-chainload/META-INF/com/google/android/updater-script" \
  "$WORK/META-INF/com/google/android/updater-script"
chmod +x "$WORK/META-INF/com/google/android/update-binary"
cp "$EFI" "$WORK/payload/gbl-chainload.efi"

cat > "$WORK/payload/manifest" <<EOF
type=gbl-chainload
efi_sha256=$(sha256sum "$EFI" | awk '{print $1}')
efi_size=$EFI_SIZE
backup_abl_sha256=$(sha256sum "$BACKUP_ABL" | awk '{print $1}')
backup_abl_expected_path=/sdcard/backup_abl.img
efisp_dest=$EFISP_DEST
EOF

python3 - "$WORK" "$OUT" <<'PY'
import os, sys, zipfile
root, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED) as z:
    for dirpath, _, filenames in os.walk(root):
        for name in filenames:
            path = os.path.join(dirpath, name)
            arc = os.path.relpath(path, root)
            z.write(path, arc)
PY

echo "wrote $OUT"
