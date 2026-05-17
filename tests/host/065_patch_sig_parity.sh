#!/usr/bin/env bash
# tests/host/065_patch_sig_parity.sh — confirm DynamicPatchLib's
# Signatures.h files all include tools/shared/patch_signatures.h, so the
# patch byte data cannot diverge between EDK2 build and host tools.
set -euo pipefail
cd "$(dirname "$0")/../.."

OUT=tests/host/.last/065
mkdir -p "$OUT"

missing=0
for f in GblChainloadPkg/Library/DynamicPatchLib/{mode_1,oem,universal}/Signatures.h; do
  if ! grep -q 'tools/shared/patch_signatures.h' "$f"; then
    echo "FAIL: $f does not include tools/shared/patch_signatures.h"
    missing=1
  fi
done
[ "$missing" = "0" ] || exit 1

# And confirm the shared header itself exists and defines kEfispUtf16Pattern.
grep -q 'kEfispUtf16Pattern' tools/shared/patch_signatures.h \
  || { echo "FAIL: kEfispUtf16Pattern missing from shared header"; exit 1; }

echo "PASS: 065 patch_sig parity"
