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
# Post engine-rework: abl-patcher always applies abl_permissive (patch6 +
# patch10); the OEM group is opt-in via `--oem`.  patch6 and patch10 anchor on
# AOSP strings that are shared with non-oplus Qcom builds, so they apply on
# Xiaomi too — that is expected and asserted below.
#
# Pass-2 idempotency note: only patch7 is designed to detect an already-
# patched anchor and return PATCH_OK.  patch10 and patch6 do not have an
# already-applied check, so re-running abl-patcher against an already-patched
# PE produces mandatory MISSes for those two and exits non-zero.  The test
# therefore runs pass 2 with `|| true` and asserts only what's invariant:
# patch7 reports OK again (idempotency) and the test still validates patch7
# anchoring, not the full mandatory contract.
#
# Uses the tracked tests/images/ ABL fixtures, so it runs in CI (not SKIP).
set -euo pipefail
cd "$(dirname "$0")/../.."

cargo build --release --quiet -p gbl
PATH="$PWD/target/release:$PATH"; export PATH
FV=(gbl unwrap)
PATCHER=(gbl patch)

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
  "${FV[@]}" "$1" "$2" >"$OUT/$(basename "$1").unwrap.log" 2>&1 \
      || { echo "FAIL: fv-unwrap $1"; cat "$OUT/$(basename "$1").unwrap.log"; exit 1; }
}

for img in "${OPLUS_ABLS[@]}"; do
  [ -f "$img" ] || { echo "SKIP: $img missing"; continue; }
  ran=1
  name=$(basename "$img" .img)
  pe="$OUT/$name.pe.efi"
  p1="$OUT/$name.p1.efi"
  unwrap "$img" "$pe"

  # 1. first application: patch7 (oem) + patch10 + patch6 must all -> OK
  "${PATCHER[@]}" --in "$pe" --out "$p1" --oem oplus \
      >"$OUT/$name.p1.log" 2>&1 \
      || { echo "FAIL: $name abl-patcher (pass 1)"; cat "$OUT/$name.p1.log"; exit 1; }
  for need in 'patch7-orange-screen .* -> OK' \
              'patch10-libavb-force-avb-success .* -> OK' \
              'patch6-lock-state-fastboot-gate .* -> OK'; do
    if ! grep -qE "$need" "$OUT/$name.p1.log"; then
      echo "FAIL: $name pass 1 missing match: $need"; cat "$OUT/$name.p1.log"; exit 1
    fi
  done

  # 2. patch7 idempotency: re-run against the already-patched PE.  patch10 +
  #    patch6 lack an already-applied check and will mandatory-miss, so the
  #    abl-patcher process itself exits non-zero — that's expected here.  The
  #    invariant we're locking in is "patch7 sees its already-rewritten guard
  #    and reports OK again" (no double-mutation, no spurious MISS).
  "${PATCHER[@]}" --in "$p1" --oem oplus \
      >"$OUT/$name.p2.log" 2>&1 \
      || true
  if ! grep -qE 'patch7-orange-screen .* -> OK' "$OUT/$name.p2.log"; then
    echo "FAIL: $name patch7 not idempotent on re-apply"
    cat "$OUT/$name.p2.log"; exit 1
  fi
  echo "  ok: $name — patch7 + patch10 + patch6 applied; patch7 idempotent"
done

for img in "${NONOPLUS_ABLS[@]}"; do
  [ -f "$img" ] || { echo "SKIP: $img missing"; continue; }
  ran=1
  name=$(basename "$img" .img)
  pe="$OUT/$name.pe.efi"
  unwrap "$img" "$pe"
  "${PATCHER[@]}" --in "$pe" --out "$OUT/$name.p.efi" --oem oplus \
      >"$OUT/$name.log" 2>&1 \
      || { echo "FAIL: $name abl-patcher returned non-zero"; cat "$OUT/$name.log"; exit 1; }
  # patch7 must MISS on non-oplus (no false positive on the oem anchor).
  if grep -qE 'patch7-orange-screen .* -> OK' "$OUT/$name.log"; then
    echo "FAIL: $name — patch7 false-positive on non-oplus ABL"; cat "$OUT/$name.log"; exit 1
  fi
  if ! grep -qE 'patch7-orange-screen .* -> MISS' "$OUT/$name.log"; then
    echo "FAIL: $name — patch7 expected MISS line not present"; cat "$OUT/$name.log"; exit 1
  fi
  # patch6 / patch10 anchor on AOSP strings shared with non-oplus Qcom builds,
  # so they apply on Xiaomi too. Lock that in: any regression that breaks the
  # AOSP-shared anchors would surface here.
  for need in 'patch10-libavb-force-avb-success .* -> OK' \
              'patch6-lock-state-fastboot-gate .* -> OK'; do
    if ! grep -qE "$need" "$OUT/$name.log"; then
      echo "FAIL: $name — abl_permissive expected OK on non-oplus: $need"
      cat "$OUT/$name.log"; exit 1
    fi
  done
  echo "  ok: $name — patch7 MISS (non-oplus); abl_permissive (patch6+patch10) OK"
done

[ "$ran" -eq 1 ] || { echo "SKIP: 088 — no ABL fixtures present"; exit 0; }

echo "PASS: 088 patch7 multi-abl"
