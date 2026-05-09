#!/usr/bin/env bash
# tests/042_dynamic_patch_harness.sh — single entry point that:
#   - builds the host scan tests (tests/scan)
#   - builds the host patch tests (tests/patches)
#   - builds tools/abl-patcher
#   - runs anchor-uniqueness check via abl-patcher --check-anchors-only against every available fixture
#   - reports a non-zero exit if any mandatory anchor fails or any host test fails
#
# Fixtures considered (presence determines coverage):
#   - images/infiniti/LinuxLoader_infiniti.efi (gbl_root_canoe symlink, primary)
#   - images/infiniti-EU-16.0.5.703/abl.bin (raw FV — exercised but failures non-fatal)
#   - images/fixtures/canoe-A.07/abl_a.bin (canoe stock; mandatory once present)
set -euo pipefail

cd "$(dirname "$0")/.."

# 1. Host scan tests.
echo "== tests/scan =="
make -C tests/scan clean >/dev/null
make -C tests/scan

# 2. Host patch tests.
echo "== tests/patches =="
make -C tests/patches clean >/dev/null
make -C tests/patches

# 3. Build the host abl-patcher binary.
echo "== tools/abl-patcher build =="
make -C tools/abl-patcher clean >/dev/null
make -C tools/abl-patcher

ABL_PATCHER=tools/abl-patcher/abl-patcher

# 4. Anchor-uniqueness check on each available fixture.
declare -a INFINITI_FIXTURES=(
  "images/infiniti/LinuxLoader_infiniti.efi"
)
# Optional: EU 16.0.5.703 raw FV — informational, no hard fail (raw FV != unwrapped PE).
declare -a OPTIONAL_FIXTURES=(
  "images/infiniti-EU-16.0.5.703/abl.bin"
)
# Canoe — mandatory if present.
CANOE_FIXTURE="images/fixtures/canoe-A.07/abl_a.bin"

FAIL=0

# Mandatory infiniti fixtures.
for fix in "${INFINITI_FIXTURES[@]}"; do
  if [[ -f "$fix" || -L "$fix" ]]; then
    echo "== anchor-uniqueness: $fix =="
    if ! "$ABL_PATCHER" --in "$fix" --check-anchors-only; then
      echo "FAIL: anchor-uniqueness on $fix"
      FAIL=1
    fi
  else
    echo "ERROR: required infiniti fixture missing: $fix"
    FAIL=1
  fi
done

# Canoe fixture: when present, mandatory.
if [[ -f "$CANOE_FIXTURE" ]]; then
  echo "== anchor-uniqueness: $CANOE_FIXTURE =="
  if ! "$ABL_PATCHER" --in "$CANOE_FIXTURE" --check-anchors-only; then
    echo "FAIL: anchor-uniqueness on $CANOE_FIXTURE"
    FAIL=1
  fi
else
  echo "WARN: canoe fixture absent — patch9 canoe leg uncovered. Drop a stock"
  echo "      A.07 abl_a.bin into images/canoe-stock-A.07_2024_02_05/ and run"
  echo "      ./scripts/extract-canoe-fixtures.sh to populate."
fi

# Optional fixtures (informational only — never fail).
for fix in "${OPTIONAL_FIXTURES[@]}"; do
  if [[ -f "$fix" || -L "$fix" ]]; then
    echo "== informational scan: $fix =="
    "$ABL_PATCHER" --in "$fix" --check-anchors-only || \
      echo "INFO: anchor-uniqueness MISS on $fix (raw FV — non-fatal)"
  fi
done

if [[ $FAIL -ne 0 ]]; then
  echo "FAIL 042_dynamic_patch_harness"
  exit 1
fi
echo "ok 042_dynamic_patch_harness"
