#!/usr/bin/env bash
# 087_mode2_profile_regression.sh — TOML byte-identical regression for the
# mode2-profile derive command. Locks down the AvbParseLib migration
# (avb-parser-consolidation feature branch, Task 3) by diffing against a
# captured golden TOML on tests/images/vbmeta-infiniti-IN-16.0.7.201.img.
#
# PR2 Task 8 inlined the former
# `tools/mode2-profile/tests/regression-fixture.sh` into this test
# since the host C tool dir was deleted; the golden moved to
# `tests/host/goldens/087/baseline.toml`.
set -euo pipefail
cd "$(dirname "$0")/../.."

FIXTURE_REL="tests/images/vbmeta-infiniti-IN-16.0.7.201.img"
GOLDEN="tests/host/goldens/087/baseline.toml"
NEW="tests/host/.last/087/derived.toml"

if [[ ! -f "$FIXTURE_REL" ]]; then
  echo "SKIP: fixture missing ($FIXTURE_REL)"; exit 0
fi
if [[ ! -f "$GOLDEN" ]]; then
  echo "FAIL: golden baseline missing ($GOLDEN)"; exit 1
fi

cargo build --release --quiet -p gbl
PATH="$PWD/target/release:$PATH"; export PATH

mkdir -p "$(dirname "$NEW")"
# Run from repo root so the vbmeta_path argument matches the baseline
# (captured as a relative path).
gbl mode2 derive "$FIXTURE_REL" -o "$NEW" >/dev/null

if diff -q "$GOLDEN" "$NEW" >/dev/null; then
  echo "PASS: mode2-profile TOML output byte-identical to baseline"
else
  echo "FAIL: mode2-profile TOML diverged from baseline"
  diff "$GOLDEN" "$NEW" || true
  exit 1
fi
