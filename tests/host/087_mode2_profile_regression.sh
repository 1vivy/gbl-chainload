#!/usr/bin/env bash
# 087_mode2_profile_regression.sh — TOML byte-identical regression for the
# mode2-profile derive command. Locks down the AvbParseLib migration
# (avb-parser-consolidation feature branch, Task 3) by diffing against a
# captured golden TOML on tests/images/vbmeta-infiniti-IN-16.0.7.201.img.
#
# Delegates to tools/mode2-profile/tests/regression-fixture.sh which lives
# next to the tool it exercises.
set -euo pipefail
cd "$(dirname "$0")/../.."  # repo root
bash tools/mode2-profile/tests/regression-fixture.sh
