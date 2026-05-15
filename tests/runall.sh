#!/usr/bin/env bash
# runall.sh — full host CI surface.
set -euo pipefail
cd "$(dirname "$0")/.."

echo "== tests/avb =="
make -C tests/avb clean >/dev/null
make -C tests/avb

echo "== 045_mode_taxonomy_lint =="
bash tests/045_mode_taxonomy_lint.sh
echo "== 046_mode1_protocol_hook_lint =="
bash tests/046_mode1_protocol_hook_lint.sh
echo "== 047_cleanup_lint =="
bash tests/047_cleanup_lint.sh
echo "== 042_dynamic_patch_harness =="
bash tests/042_dynamic_patch_harness.sh
echo "== 051_gbl_root_canoe_regression =="
bash tests/051_gbl_root_canoe_regression.sh
echo "== 053_cache_abl_lint =="
bash tests/053_cache_abl_lint.sh
echo "== 054_gbl_zip_lint =="
bash tests/054_gbl_zip_lint.sh
echo "== 055_recovery_graft_lint =="
bash tests/055_recovery_graft_lint.sh

# Carried-forward signature lint, if present.
if [[ -f tests/030_signature_lint.sh ]]; then
  echo "== 030_signature_lint =="
  bash tests/030_signature_lint.sh
fi

# Build smoke is slowest — last.
echo "== 010_build_smoke =="
bash tests/010_build_smoke.sh

echo "ALL TESTS PASS"
