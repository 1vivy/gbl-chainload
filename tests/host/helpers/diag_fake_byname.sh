#!/usr/bin/env bash
# tests/host/helpers/diag_fake_byname.sh — build a synthetic by-name dir
# plus a staged recovery environment so diag.sh can run on the host.
#
# Usage: diag_fake_byname.sh <out-root> <scenario>
#   <scenario>: high | medium | low | none
set -euo pipefail

SELF="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SELF/../../.." && pwd)"

OUT="$1"
SCENARIO="$2"
rm -rf "$OUT"
mkdir -p "$OUT/byname" "$OUT/work" "$OUT/sdcard" "$OUT/zip"

ZIP_ROOT="$REPO/zip"
# Use zip/base/mode-1.efi as the base-EFI prefix for EFISP.  With Fix A in
# gblp1-inspect (magic+CRC validated together), the embedded GBLP1 string
# inside mode-1.efi is skipped and only the appended real GBLP1 container is
# selected.  Using the real base EFI also lets collect_efisp fingerprint it
# against MANIFEST and populate BASE_EFI_MODE=mode-1, satisfying §4.1 HIGH.
BASE_EFI="$REPO/zip/base/mode-1.efi"
[ -f "$BASE_EFI" ] || { echo "ERROR: $BASE_EFI not found" >&2; exit 1; }

# Canonical payload: prefer 060 output, fall back to 084 output.
PAYLOAD=""
for candidate in \
    "$REPO/tests/host/.last/060/payload.bin" \
    "$REPO/tests/host/.last/084/payload.bin"; do
  if [ -f "$candidate" ]; then
    PAYLOAD="$candidate"
    break
  fi
done
[ -n "$PAYLOAD" ] || { echo "ERROR: no payload.bin found; run 060 or 084 first" >&2; exit 1; }

# Patched PE for ABL slots that should NOT retain the loader path.
PATCHED_PE="$REPO/tests/host/.last/060/patched.efi"
if [ ! -f "$PATCHED_PE" ]; then
  PATCHED_PE="$REPO/tests/host/.last/084/patched.efi"
fi
[ -f "$PATCHED_PE" ] || { echo "ERROR: no patched.efi found" >&2; exit 1; }

# Original PE for ABL slots that SHOULD retain the loader path.
ORIG_PE="$REPO/tests/images/pe/infiniti-EU-16.0.5.703.efi"
[ -f "$ORIG_PE" ] || { echo "ERROR: original PE $ORIG_PE not found" >&2; exit 1; }

# 1. EFISP fixture.
case "$SCENARIO" in
  high|medium)
    # base EFI + valid GBLP1 payload.
    cat "$BASE_EFI" "$PAYLOAD" > "$OUT/byname/efisp"
    ;;
  low)
    # PE (MZ present) but no valid GBLP1 — just the base EFI with random suffix.
    cp "$BASE_EFI" "$OUT/byname/efisp"
    dd if=/dev/urandom bs=64 count=1 >> "$OUT/byname/efisp" 2>/dev/null
    ;;
  none)
    # Not a PE — random bytes that don't start with MZ.
    dd if=/dev/urandom bs=4096 count=1 2>/dev/null | \
      python3 -c 'import sys; b=bytearray(sys.stdin.buffer.read()); b[0]=0x00; b[1]=0x00; sys.stdout.buffer.write(b)' \
      > "$OUT/byname/efisp"
    ;;
  *)
    echo "ERROR: unknown scenario '$SCENARIO'" >&2; exit 1 ;;
esac

# 2. ABL fixtures.
#    high  → abl_a has loader path (original PE), abl_b does not (patched PE).
#    other → neither slot has loader path (patched PE).
case "$SCENARIO" in
  high)
    cp "$ORIG_PE" "$OUT/byname/abl_a"
    cp "$PATCHED_PE" "$OUT/byname/abl_b"
    ;;
  medium|low|none)
    cp "$PATCHED_PE" "$OUT/byname/abl_a"
    cp "$PATCHED_PE" "$OUT/byname/abl_b"
    ;;
esac

# 3. vbmeta + logfs stubs — random bytes (descriptors/history not asserted
#    beyond file presence in this dry-run test).
dd if=/dev/urandom bs=65536 count=1 2>/dev/null > "$OUT/byname/vbmeta_a"
dd if=/dev/urandom bs=65536 count=1 2>/dev/null > "$OUT/byname/vbmeta_b"
dd if=/dev/urandom bs=65536 count=1 2>/dev/null > "$OUT/byname/logfs"

# 4. Stage the zip/ tree so $WORKDIR/bin/MANIFEST resolves correctly.
cp -r "$ZIP_ROOT/." "$OUT/zip/"
