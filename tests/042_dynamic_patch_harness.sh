#!/usr/bin/env bash
# tests/042_dynamic_patch_harness.sh — single entry point that:
#   - builds the host scan tests (tests/scan)
#   - builds the host patch tests (tests/patches)
#   - builds tools/abl-patcher
#   - runs anchor-uniqueness check via abl-patcher --check-anchors-only against
#     every ABL fixture present in tests/images/
#   - reports a non-zero exit if any anchor-uniqueness check fails on a present
#     fixture
#
# Fixture discovery: globs tests/images/*.{efi,bin,img}. Override via env:
#   FIXTURES_DIR=/path/to/blobs ./tests/042_dynamic_patch_harness.sh
#
# Fixture blobs are gitignored device firmware — locally the user drops them
# into tests/images/; CI runs with whatever is committed there.
set -euo pipefail

cd "$(dirname "$0")/.."

FIXTURES_DIR="${FIXTURES_DIR:-tests/images}"

# 1. Host scan tests.
echo "== tests/scan =="
make -C tests/scan clean >/dev/null
make -C tests/scan

# 2. Host patch tests.
echo "== tests/patches =="
make -C tests/patches clean >/dev/null
make -C tests/patches TEST_FIXTURES_DIR="$(realpath "$FIXTURES_DIR")"

# 3. Build the host abl-patcher binary.
echo "== tools/abl-patcher build =="
make -C tools/abl-patcher clean >/dev/null
make -C tools/abl-patcher

ABL_PATCHER=tools/abl-patcher/abl-patcher

# 4. Anchor-uniqueness check.
#
# Split fixtures by extension:
#   *.efi  — extracted PE form. Patches' PE-section gates engage; anchor
#            miss/ambiguous here is a real failure → fail the harness.
#   *.bin, *.img — raw FV wrappers. Patches' PE gates won't engage. Report
#            informationally; failures are non-fatal until an FV→PE
#            extractor lands (see scripts/extract-pe-from-fv.sh /
#            tools/fv-unwrap follow-up).
shopt -s nullglob
PE_FIXTURES=( "$FIXTURES_DIR"/*.efi )
FV_FIXTURES=( "$FIXTURES_DIR"/*.bin "$FIXTURES_DIR"/*.img )
shopt -u nullglob

FAIL=0
for fix in "${PE_FIXTURES[@]}"; do
  echo "== anchor-uniqueness (mandatory): $fix =="
  if ! "$ABL_PATCHER" --in "$fix" --check-anchors-only; then
    echo "FAIL: anchor-uniqueness on $fix"
    FAIL=1
  fi
done

for fix in "${FV_FIXTURES[@]}"; do
  echo "== anchor-uniqueness (informational, raw FV): $fix =="
  "$ABL_PATCHER" --in "$fix" --check-anchors-only || \
    echo "INFO: anchor-uniqueness MISS on $fix (raw FV — non-fatal; needs PE extraction)"
done

TOTAL=$(( ${#PE_FIXTURES[@]} + ${#FV_FIXTURES[@]} ))
if [[ $TOTAL -eq 0 ]]; then
  echo "WARN: no ABL fixtures found in $FIXTURES_DIR — anchor-uniqueness uncovered"
  echo "ok 042_dynamic_patch_harness (no fixture coverage)"
  exit 0
fi

if [[ $FAIL -ne 0 ]]; then
  echo "FAIL 042_dynamic_patch_harness"
  exit 1
fi
echo "ok 042_dynamic_patch_harness (${#PE_FIXTURES[@]} PE + ${#FV_FIXTURES[@]} FV exercised)"
