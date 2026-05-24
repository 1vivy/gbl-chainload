#!/usr/bin/env bash
# tests/042_dynamic_patch_harness.sh — single entry point that:
#   - runs the Rust patch-engine unit + parity tests (replaces the
#     deleted tests/scan + tests/patches C harnesses)
#   - builds tools/abl-patcher
#   - runs anchor-uniqueness check via abl-patcher --check-anchors-only
#     against every ABL fixture present in tests/images/
#   - reports a non-zero exit if any anchor-uniqueness check fails on a
#     present fixture
#
# Fixture discovery: globs tests/images/*.{efi,bin,img}. Override via env:
#   FIXTURES_DIR=/path/to/blobs ./tests/042_dynamic_patch_harness.sh
#
# Fixture blobs are gitignored device firmware — locally the user drops
# them into tests/images/; CI runs with whatever is committed there.
#
# PR2 Task 6: the engine moved into crates/patch-engine (Rust). The C
# harnesses under tests/scan/ + tests/patches/ were deleted because the
# parity is now in crates/patch-engine/tests/parity.rs + the in-module
# unit tests.
set -euo pipefail

cd "$(dirname "$0")/.."

FIXTURES_DIR="${FIXTURES_DIR:-tests/images}"

# 1. Rust patch-engine tests (unit + parity).
echo "== cargo test -p patch-engine =="
cargo test --release -p patch-engine --features host

# 2. Build the gbl multicall binary (former tools/abl-patcher is now
# `gbl patch`).
echo "== gbl multicall build =="
cargo build --release --quiet -p gbl

ABL_PATCHER=(./target/release/gbl patch)

# 3. Anchor-uniqueness check.
#
# Split fixtures by extension:
#   *.efi  — extracted PE form. Patches' PE-section gates engage;
#            anchor miss/ambiguous here is a real failure → fail the
#            harness.
#   *.bin, *.img — raw FV wrappers. Patches' PE gates won't engage.
#            Report informationally; failures are non-fatal until an
#            FV→PE extractor lands (see scripts/extract-pe-from-fv.sh /
#            tools/fv-unwrap follow-up).
shopt -s nullglob
PE_FIXTURES=( "$FIXTURES_DIR"/*.efi )
FV_FIXTURES=( "$FIXTURES_DIR"/*.bin "$FIXTURES_DIR"/*.img )
shopt -u nullglob

FAIL=0
for fix in "${PE_FIXTURES[@]}"; do
  echo "== anchor-uniqueness (mandatory): $fix =="
  if ! "${ABL_PATCHER[@]}" --in "$fix" --check-anchors-only; then
    echo "FAIL: anchor-uniqueness on $fix"
    FAIL=1
  fi
done

for fix in "${FV_FIXTURES[@]}"; do
  echo "== anchor-uniqueness (informational, raw FV): $fix =="
  "${ABL_PATCHER[@]}" --in "$fix" --check-anchors-only || \
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
