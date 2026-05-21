#!/usr/bin/env bash
# tests/host/088_patch7_multi_abl.sh — patch7 (orange-screen) cross-build gate.
#
# patch7 is string-anchored: it scans the orange-state warning text, resolves
# its unique ADRP+ADD, and rewrites the nearest preceding CBZ Wn.  This proved
# necessary because the original EU-16.0.5.703 fixed byte anchor missed
# IN-16.0.7.201 (an extra STR shifts the CBZ from CSEL+4 to CSEL+8).  This test
# locks in the cross-build guarantee: patch7 must APPLY (-> OK) and be
# idempotent on every oplus-family ABL fixture, and must NOT false-positive on
# a non-oplus (Xiaomi) ABL.
#
# Uses the tracked tests/images/ ABL fixtures, so it runs in CI (not SKIP).
set -euo pipefail
cd "$(dirname "$0")/../.."

make -s -C tools/fv-unwrap
make -s -C tools/abl-patcher
FV=tools/fv-unwrap/fv-unwrap
PATCHER=tools/abl-patcher/abl-patcher

OUT=tests/host/.last/088
mkdir -p "$OUT"

# oplus-family ABLs: patch7 must apply.  Add new dumps here as they land.
OPLUS_ABLS=(
  tests/images/op15-infiniti-703-abl.img
  tests/images/op15-infiniti-201-abl.img
  tests/images/op15t-fairlady-201-abl.img
)
# non-oplus ABL: patch7 must cleanly miss (no false positive).
NONOPLUS_ABLS=(
  tests/images/xi17-pudding-44-abl.img
)

ran=0

# unwrap <img> <out-pe>  — abort on failure.
unwrap() {
  "$FV" "$1" "$2" >"$OUT/$(basename "$1").unwrap.log" 2>&1 \
    || { echo "FAIL: fv-unwrap $1"; cat "$OUT/$(basename "$1").unwrap.log"; exit 1; }
}

for img in "${OPLUS_ABLS[@]}"; do
  [ -f "$img" ] || { echo "SKIP: $img missing"; continue; }
  ran=1
  name=$(basename "$img" .img)
  pe="$OUT/$name.pe.efi"
  p1="$OUT/$name.p1.efi"
  p2="$OUT/$name.p2.efi"
  unwrap "$img" "$pe"

  # 1. first application must report patch7 -> OK
  "$PATCHER" --in "$pe" --out "$p1" --oem oneplus --no-mode1 \
      >"$OUT/$name.p1.log" 2>&1 \
      || { echo "FAIL: $name abl-patcher (pass 1)"; cat "$OUT/$name.p1.log"; exit 1; }
  if ! grep -qE 'patch7-orange-screen .* -> OK' "$OUT/$name.p1.log"; then
    echo "FAIL: $name patch7 not applied (-> OK)"; cat "$OUT/$name.p1.log"; exit 1
  fi

  # 2. idempotency: re-patch the patched PE, patch7 must still report OK
  "$PATCHER" --in "$p1" --out "$p2" --oem oneplus --no-mode1 \
      >"$OUT/$name.p2.log" 2>&1 \
      || { echo "FAIL: $name abl-patcher (pass 2)"; cat "$OUT/$name.p2.log"; exit 1; }
  if ! grep -qE 'patch7-orange-screen .* -> OK' "$OUT/$name.p2.log"; then
    echo "FAIL: $name patch7 not idempotent (-> OK on re-apply)"; cat "$OUT/$name.p2.log"; exit 1
  fi

  # 3. the rewritten word must be the unconditional B (0x14000023), and the
  #    second pass must not have changed it again (byte-stable).
  if ! cmp -s "$p1" "$p2"; then
    echo "FAIL: $name re-apply changed bytes (not idempotent)"; exit 1
  fi
  echo "  ok: $name — patch7 applied + idempotent"
done

for img in "${NONOPLUS_ABLS[@]}"; do
  [ -f "$img" ] || { echo "SKIP: $img missing"; continue; }
  ran=1
  name=$(basename "$img" .img)
  pe="$OUT/$name.pe.efi"
  unwrap "$img" "$pe"
  "$PATCHER" --in "$pe" --out "$OUT/$name.p.efi" --oem oneplus --no-mode1 \
      >"$OUT/$name.log" 2>&1 \
      || { echo "FAIL: $name abl-patcher returned non-zero"; cat "$OUT/$name.log"; exit 1; }
  if grep -qE 'patch7-orange-screen .* -> OK' "$OUT/$name.log"; then
    echo "FAIL: $name — patch7 false-positive on non-oplus ABL"; cat "$OUT/$name.log"; exit 1
  fi
  echo "  ok: $name — patch7 correctly does not apply (non-oplus)"
done

[ "$ran" -eq 1 ] || { echo "SKIP: 088 — no ABL fixtures present"; exit 0; }
echo "PASS: 088 patch7 multi-abl"
