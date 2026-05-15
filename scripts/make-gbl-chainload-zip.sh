#!/usr/bin/env bash
# Build a host-generated recovery ZIP for a cache-ABL gbl-chainload EFI.
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage: scripts/make-gbl-chainload-zip.sh \
  --efi dist/mode-1-cache-abl.efi \
  --out dist/gbl-chainload.zip \
  [--efisp-dest gbl-chainload.efi]

The recovery installer assumes the OTA ZIP has just updated the inactive ABL
slot. It expects /sdcard/backup_abl.img on the device to be a whole, known-good,
GBL-capable ABL image. The installer writes the EFI to EFISP, backs up the
inactive-slot OTA ABL, then restores backup_abl.img to that inactive ABL slot so
it can keep loading GBL apps.
EOF
  exit 2
}

EFI=""
OUT=""
EFISP_DEST="gbl-chainload.efi"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --efi) EFI="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --efisp-dest) EFISP_DEST="$2"; shift 2 ;;
    -h|--help) usage ;;
    *) echo "unknown arg: $1" >&2; usage ;;
  esac
done

[[ -n "$EFI" && -n "$OUT" ]] || usage
[[ -f "$EFI" ]] || { echo "missing EFI: $EFI" >&2; exit 2; }

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
