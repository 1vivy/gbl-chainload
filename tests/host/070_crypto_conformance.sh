#!/usr/bin/env bash
# tests/host/070_crypto_conformance.sh — known-answer tests for the
# vendored crypto primitives (gbl_sha256, gbl_crc32).
#
# These primitives are vendored as a single source compiled into the EFI
# shim and both host/Android gbl-pack builds. The KAT vectors pin them to
# the SHA-256 (FIPS 180-4) and CRC-32 (IEEE 802.3) standards, so producer
# (gbl-pack) and consumer (the shim) are guaranteed to agree.
set -euo pipefail
cd "$(dirname "$0")/../.."

make -s -C tests/host/helpers test_crc32 test_sha256
OUT=tests/host/.last/070
mkdir -p "$OUT"

rc=0
for t in test_crc32 test_sha256; do
  if tests/host/helpers/$t >"$OUT/$t.log" 2>&1; then
    echo "PASS: 070 $t"
  else
    echo "FAIL: 070 $t"
    cat "$OUT/$t.log"
    rc=1
  fi
done
exit $rc
