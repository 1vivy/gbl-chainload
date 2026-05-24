#!/usr/bin/env bash
# tests/host/083_abl_patcher_oem.sh — abl-patcher --oem behaviour.
#
# Engine-rework spec (Task 12): abl_permissive is ALWAYS applied at host
# packing time; --no-mode1 / --no-libavb-bypass are gone.  --oem oplus is
# canonical, --oem oneplus is a deprecation alias that still maps to
# GBL_OEM_OPLUS for one release.
#
# Cases:
#   1. --oem oplus      canonical OEM scope; patch7 applies.
#   2. --oem oneplus    deprecation-message alias for oplus; patch7 applies.
#   3. plain            abl_permissive always on; patch10 + patch6 apply.
#   4. --oem bad        exits non-zero with "unknown --oem '<value>'".
# SKIP-guarded if the PE fixture is absent.
set -euo pipefail
cd "$(dirname "$0")/../.."

PE=tests/images/pe/infiniti-EU-16.0.5.703.efi
[ -f "$PE" ] || { echo "SKIP: $PE missing — run scripts/extract-pe-from-fv.sh first" >&2; exit 0; }

cargo build --release --quiet -p gbl
PATH="$PWD/target/release:$PATH"; export PATH
PATCHER=(gbl patch)

OUT=tests/host/.last/083
mkdir -p "$OUT"

# ---- Case 1: --oem oplus (canonical) -----------------------------------------
"${PATCHER[@]}" --in "$PE" --oem oplus --out "$OUT/oplus.efi" >"$OUT/oplus.log" 2>&1 \
    || { echo "FAIL: abl-patcher --oem oplus returned non-zero"; cat "$OUT/oplus.log"; exit 1; }
if ! grep -qE 'patch7-orange-screen .* -> OK' "$OUT/oplus.log"; then
    echo "FAIL: oem patch7 not applied (-> OK) under --oem oplus"
    cat "$OUT/oplus.log"
    exit 1
fi
if ! grep -qE 'patch10-libavb-force-avb-success .* -> OK' "$OUT/oplus.log"; then
    echo "FAIL: abl_permissive patch10 not applied under --oem oplus"
    cat "$OUT/oplus.log"
    exit 1
fi
if ! grep -qE 'patch6-lock-state-fastboot-gate .* -> OK' "$OUT/oplus.log"; then
    echo "FAIL: abl_permissive patch6 not applied under --oem oplus"
    cat "$OUT/oplus.log"
    exit 1
fi
echo "  ok: --oem oplus applies patch7 + abl_permissive patches"

# ---- Case 2: --oem oneplus (deprecation alias) -------------------------------
"${PATCHER[@]}" --in "$PE" --oem oneplus --out "$OUT/oneplus.efi" >"$OUT/oneplus.log" 2>&1 \
    || { echo "FAIL: abl-patcher --oem oneplus returned non-zero"; cat "$OUT/oneplus.log"; exit 1; }
if ! grep -qF 'abl-patcher: --oem oneplus is deprecated; use --oem oplus' "$OUT/oneplus.log"; then
    echo "FAIL: deprecation message missing under --oem oneplus"
    cat "$OUT/oneplus.log"
    exit 1
fi
if ! grep -qE 'patch7-orange-screen .* -> OK' "$OUT/oneplus.log"; then
    echo "FAIL: oem patch7 not applied (-> OK) under --oem oneplus alias"
    cat "$OUT/oneplus.log"
    exit 1
fi
echo "  ok: --oem oneplus prints deprecation msg, still maps to oplus"

# ---- Case 3: plain invocation always applies abl_permissive ------------------
"${PATCHER[@]}" --in "$PE" --out "$OUT/plain.efi" >"$OUT/plain.log" 2>&1 \
    || { echo "FAIL: plain abl-patcher returned non-zero"; cat "$OUT/plain.log"; exit 1; }
if ! grep -qE 'patch10-libavb-force-avb-success .* -> OK' "$OUT/plain.log"; then
    echo "FAIL: abl_permissive patch10 absent from plain run"
    cat "$OUT/plain.log"
    exit 1
fi
if ! grep -qE 'patch6-lock-state-fastboot-gate .* -> OK' "$OUT/plain.log"; then
    echo "FAIL: abl_permissive patch6 absent from plain run"
    cat "$OUT/plain.log"
    exit 1
fi
# Plain invocation must NOT pull in the OEM scope.
if grep -qF 'patch7-orange-screen' "$OUT/plain.log"; then
    echo "FAIL: oem patch7 present in plain (no --oem) run"
    cat "$OUT/plain.log"
    exit 1
fi
echo "  ok: plain invocation always applies abl_permissive (no OEM scope)"

# ---- Case 4: --oem bad rejected with exit code 2 -----------------------------
set +e
"${PATCHER[@]}" --in "$PE" --oem bad_oem_name --out "$OUT/bad.efi" >"$OUT/bad.log" 2>&1
rc=$?
set -e
if [ "$rc" -eq 0 ]; then
    echo "FAIL: --oem bad_oem_name was accepted (exit 0)"
    cat "$OUT/bad.log"
    exit 1
fi
if [ "$rc" -ne 2 ]; then
    echo "FAIL: --oem bad_oem_name expected exit 2, got $rc"
    cat "$OUT/bad.log"
    exit 1
fi
if ! grep -qF "abl-patcher: unknown --oem 'bad_oem_name'" "$OUT/bad.log"; then
    echo "FAIL: expected \"abl-patcher: unknown --oem 'bad_oem_name'\" message, got:"
    cat "$OUT/bad.log"
    exit 1
fi
echo "  ok: --oem bad_oem_name rejected with exit 2 + clear message"

# ---- Regression gate ---------------------------------------------------------
# Run sibling tests so a breakage in roundtrip / mode taxonomy surfaces here
# too.  Each test exits 0 on SKIP (missing fixture) already.
bash tests/host/060_pack_roundtrip.sh
bash tests/045_mode_taxonomy_lint.sh

echo "PASS: 083 abl-patcher --oem behavior"
