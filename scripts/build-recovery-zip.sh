#!/usr/bin/env bash
# scripts/build-recovery-zip.sh — assemble dist/gbl-chainload-installer.zip
# from the cross-compiled recovery tools + base EFI + zip/gbl-chainload/.
set -euo pipefail
cd "$(dirname "$0")/.."

# Recovery tools must be built first.
[ -d dist/recovery ] || scripts/build-recovery-tools.sh
[ -f dist/mode-1.efi ] || { echo "build dist/mode-1.efi first (scripts/build.sh --mode 1)" >&2; exit 1; }

mkdir -p zip/gbl-chainload/bin zip/gbl-chainload/base
cp dist/recovery/fv-unwrap dist/recovery/abl-patcher \
   dist/recovery/gbl-pack dist/recovery/gbl-commit \
   zip/gbl-chainload/bin/
cp dist/mode-1.efi zip/gbl-chainload/base/gbl-chainload.efi

(cd zip/gbl-chainload && \
   sha256sum bin/* base/* META-INF/com/google/android/* README.txt > SHA256SUMS && \
   rm -f "$OLDPWD/dist/gbl-chainload-installer.zip" && \
   zip -qr "$OLDPWD/dist/gbl-chainload-installer.zip" .)

echo "==> dist/gbl-chainload-installer.zip"
ls -l dist/gbl-chainload-installer.zip
unzip -l dist/gbl-chainload-installer.zip
