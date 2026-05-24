#!/usr/bin/env bash
# 087_mode2_profile_regression.sh — schema regression for the mode2-profile
# derive command on tests/images/vbmeta-infiniti-IN-16.0.7.201.img. Asserts
# the derived TOML carries the expected field values (version, lock state,
# system_version, color, spl comment). Replaces the prior byte-identical
# golden assertion now that the Rust impl is authoritative — field-level
# checks survive cosmetic format tweaks without sacrificing the regression
# coverage that mattered.
set -euo pipefail
cd "$(dirname "$0")/../.."

FIXTURE_REL="tests/images/vbmeta-infiniti-IN-16.0.7.201.img"
NEW="tests/host/.last/087/derived.toml"

if [[ ! -f "$FIXTURE_REL" ]]; then
  echo "SKIP: fixture missing ($FIXTURE_REL)"; exit 0
fi

cargo build --release --quiet -p gbl
PATH="$PWD/target/release:$PATH"; export PATH

mkdir -p "$(dirname "$NEW")"
# Run from repo root so the vbmeta_path argument matches the baseline shape
# (captured as a relative path in the # source comment).
gbl mode2 derive "$FIXTURE_REL" -o "$NEW" >/dev/null

fail() { echo "FAIL 087: $1"; cat "$NEW"; exit 1; }

# Field-level schema regression — each line was a one-liner in the prior
# baseline.toml and the values are fixture-driven (vbmeta-infiniti-201).
grep -qE '^version[[:space:]]*= 1$'                "$NEW" || fail "version != 1"
grep -qE '^is_unlocked[[:space:]]*= 0$'            "$NEW" || fail "is_unlocked != 0"
grep -qE '^color[[:space:]]*= 0$'                  "$NEW" || fail "color != 0"
grep -qE '^system_version[[:space:]]*= 0x40000$'   "$NEW" || fail "system_version != 0x40000"
grep -qE '^system_spl[[:space:]]*= 0x9a5$'         "$NEW" || fail "system_spl != 0x9a5"
grep -qE 'spl:'                                     "$NEW" || fail "spl comment missing"

echo "PASS: 087 mode2-profile schema regression"
