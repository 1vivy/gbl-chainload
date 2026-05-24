#!/usr/bin/env bash
# tests/host/093_manifest_parse.sh — manifest entry parse coverage
# (engine-rework PR1 / Task 1). Drives the C helper which exercises 8
# cases against gbl_payload_find_manifest().
set -euo pipefail
cd "$(dirname "$0")/../.."
make -s -C tests/host/helpers test_manifest_parse
exec tests/host/helpers/test_manifest_parse
