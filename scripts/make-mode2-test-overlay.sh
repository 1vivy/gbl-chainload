#!/usr/bin/env bash
# scripts/make-mode2-test-overlay.sh — build a staged-bootable mode-2 test EFI.
#
# Pipeline: build mode-2 EFI -> derive profile XML from a stock vbmeta ->
# compile to a 120-byte binary -> gbl-pack into a GBLP1 0x0010 overlay ->
# concatenate onto dist/mode-2.efi -> dist/mode-2-test.efi
#
# Usage: scripts/make-mode2-test-overlay.sh [vbmeta.img]
#   vbmeta.img defaults to images/vbmeta-infiniti-IN-16.0.7.201.img
#
# Test on device with:
#   fastboot stage dist/mode-2-test.efi
#   fastboot oem boot-efi
set -euo pipefail
cd "$(dirname "$0")/.."

VBMETA="${1:-images/vbmeta-infiniti-IN-16.0.7.201.img}"
AVBTOOL="${AVBTOOL:-$HOME/avbtool.py}"
OUT=dist/mode-2-test-build
M2P=tools/mode2-profile/mode2-profile.py

[ -f "$VBMETA" ]  || { echo "error: vbmeta not found: $VBMETA" >&2; exit 1; }
[ -f "$AVBTOOL" ] || { echo "error: avbtool.py not found at $AVBTOOL (set AVBTOOL=)" >&2; exit 1; }

mkdir -p "$OUT" dist

echo "==> Building mode-2 EFI"
./scripts/build.sh --mode 2 --debug --verbose

echo "==> Deriving profile from $VBMETA"
AVBTOOL="$AVBTOOL" python3 "$M2P" derive "$VBMETA" -o "$OUT/profile.xml"

echo "==> Compiling profile"
python3 "$M2P" compile "$OUT/profile.xml" -o "$OUT/profile.bin"

echo "==> Packing GBLP1 0x0010 overlay"
make -s -C tools/gbl-pack
tools/gbl-pack/gbl-pack --mode2-profile "$OUT/profile.bin" --out "$OUT/overlay.bin"

echo "==> Concatenating overlay onto dist/mode-2.efi"
cat dist/mode-2.efi "$OUT/overlay.bin" > dist/mode-2-test.efi

SZ=$(stat -c%s dist/mode-2-test.efi)
echo "==> dist/mode-2-test.efi ready ($SZ bytes)"
echo "    test on device:  fastboot stage dist/mode-2-test.efi && fastboot oem boot-efi"
