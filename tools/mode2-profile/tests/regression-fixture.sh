#!/usr/bin/env bash
# Regression test: mode2-profile derive output must be byte-identical to
# the captured golden TOML for tests/images/vbmeta-infiniti-IN-16.0.7.201.img.
# This locks down the host-tool migration to AvbParseLib (Task 3 of the AVB
# parser consolidation plan).
# To recapture the golden after fixture update, from repo root:
#   tools/mode2-profile/mode2-profile derive tests/images/vbmeta-infiniti-IN-16.0.7.201.img -o tools/mode2-profile/tests/baseline.toml.golden
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOL_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$TOOL_DIR/../.." && pwd)"

# The path given to mode2-profile must match what was used to capture
# the baseline (relative to repo root: "tests/images/...").
FIXTURE_REL="tests/images/vbmeta-infiniti-IN-16.0.7.201.img"
FIXTURE_ABS="$REPO_ROOT/$FIXTURE_REL"
GOLDEN="$TOOL_DIR/tests/baseline.toml.golden"
NEW="/tmp/mode2-profile-new.toml"

if [[ ! -f "$FIXTURE_ABS" ]]; then
  echo "SKIP: fixture missing ($FIXTURE_ABS)"; exit 0
fi
if [[ ! -f "$GOLDEN" ]]; then
  echo "FAIL: golden baseline missing ($GOLDEN)"; exit 1
fi

# Build from the tool directory, then run from repo root so the vbmeta_path
# argument matches the baseline (which was captured as a relative path).
(cd "$TOOL_DIR" && make >/dev/null)
(cd "$REPO_ROOT" && "$TOOL_DIR/mode2-profile" derive "$FIXTURE_REL" -o "$NEW" >/dev/null)

if diff -q "$GOLDEN" "$NEW" >/dev/null; then
  echo "PASS: mode2-profile TOML output byte-identical to baseline"
else
  echo "FAIL: mode2-profile TOML diverged from baseline"
  diff "$GOLDEN" "$NEW" || true
  exit 1
fi
