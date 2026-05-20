#!/usr/bin/env bash
# tests/host/083_abl_patcher_oem.sh — abl-patcher --oem runtime scope selection.
#
# Checks:
#   1. --oem oneplus --no-mode1  applies universal + oneplus; mode_1 patches absent.
#   2. plain invocation          applies mode_1 patches (present in stderr).
#   3. --oem bad                 exits non-zero with "error: unknown --oem".
# SKIP-guarded if the PE fixture is absent (same fixture as 060).
#
# Coverage note: this test exercises --oem *routing* (the scope-selection path
# through EnsureInitScoped), but cannot positively verify OEM-patch *application*
# because the OnePlus OEM group (oem/oneplus_canoe.c) is currently empty — the
# only OEM patch (patch7-orange-screen) is behind an unset GBL_PATCH7_ENABLED.
# Once GBL_PATCH7_ENABLED is enabled, add a positive grep for that patch name in
# the --oem oneplus run to close this coverage gap.
set -euo pipefail
cd "$(dirname "$0")/../.."

PE=tests/images/pe/infiniti-EU-16.0.5.703.efi
[ -f "$PE" ] || { echo "SKIP: $PE missing — run scripts/extract-pe-from-fv.sh first" >&2; exit 0; }

make -s -C tools/abl-patcher
PATCHER=tools/abl-patcher/abl-patcher

OUT=tests/host/.last/083
mkdir -p "$OUT"

# ---- Test 1: --oem oneplus --no-mode1 ----------------------------------------
# mode_1 patches (patch10, patch6) must NOT appear in the per-patch log lines.
"$PATCHER" --in "$PE" --oem oneplus --no-mode1 \
    --out "$OUT/m2_patched.efi" >"$OUT/m2.log" 2>&1 \
    || { echo "FAIL: abl-patcher --oem oneplus --no-mode1 returned non-zero"; cat "$OUT/m2.log"; exit 1; }

if grep -qF 'patch10-libavb-force-avb-success' "$OUT/m2.log"; then
    echo "FAIL: mode_1 patch10 present in --no-mode1 run"
    cat "$OUT/m2.log"
    exit 1
fi
if grep -qF 'patch6-lock-state-fastboot-gate' "$OUT/m2.log"; then
    echo "FAIL: mode_1 patch6 present in --no-mode1 run"
    cat "$OUT/m2.log"
    exit 1
fi
echo "  ok: --oem oneplus --no-mode1 excludes mode_1 patches"

# ---- Test 2: default (mode-1) invocation -------------------------------------
# mode_1 patches MUST appear.
"$PATCHER" --in "$PE" --out "$OUT/m1_patched.efi" >"$OUT/m1.log" 2>&1 \
    || { echo "FAIL: plain abl-patcher returned non-zero"; cat "$OUT/m1.log"; exit 1; }

if ! grep -qF 'patch10-libavb-force-avb-success' "$OUT/m1.log"; then
    echo "FAIL: mode_1 patch10 absent from default (mode-1) run"
    cat "$OUT/m1.log"
    exit 1
fi
if ! grep -qF 'patch6-lock-state-fastboot-gate' "$OUT/m1.log"; then
    echo "FAIL: mode_1 patch6 absent from default (mode-1) run"
    cat "$OUT/m1.log"
    exit 1
fi
echo "  ok: plain invocation includes mode_1 patches"

# ---- Test 3: unknown --oem must exit non-zero --------------------------------
if "$PATCHER" --in "$PE" --oem bad_oem_name 2>"$OUT/bad_oem.log"; then
    echo "FAIL: --oem bad_oem_name should have exited non-zero"
    exit 1
fi
if ! grep -qF "error: unknown --oem 'bad_oem_name'" "$OUT/bad_oem.log"; then
    echo "FAIL: expected 'error: unknown --oem' message, got:"
    cat "$OUT/bad_oem.log"
    exit 1
fi
echo "  ok: unknown --oem exits non-zero with clear message"

# ---- Regression gate --------------------------------------------------------
# Run sibling tests so a breakage in roundtrip / efisp-scan / mode taxonomy
# surfaces here too.  Each test exits 0 on SKIP (missing fixture) already.
bash tests/host/060_pack_roundtrip.sh
bash tests/host/062_efisp_scan_gate.sh
bash tests/045_mode_taxonomy_lint.sh

echo "PASS: 083 abl-patcher oem"
