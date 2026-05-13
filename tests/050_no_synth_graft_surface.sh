#!/usr/bin/env bash
# 050_no_synth_graft_surface.sh — regression check.
#
# The synth/graft on-device fastboot surface (oem synthesize-and-flash,
# oem graft-from-staged, oem fix-vbmeta-footer) was an experiment that
# never landed on main. The host helper that fed it (scripts/synthesize-
# vbmeta.py) was reverted from main by PR #13 on 2026-05-12.
#
# Historical context (preserved here in lieu of memory-note banners):
#
#   Trajectory abandoned — the three command literals above lived only on
#   the edk2 submodule branch fastboot/synthesize-and-flash and the
#   main-repo feature branch feature/synthesize-fastboot-cmd. The on-device
#   consumer (PR C of 3 of that series) never landed on main. PR #13
#   reverted the stranded host helper (scripts/synthesize-vbmeta.py) and
#   its roundtrip test (tests/053_synthesize_vbmeta_roundtrip.sh) from main
#   on 2026-05-12, leaving the tree clean.
#
#   The graft technique itself (stock OEM vbmeta bytes pasted at
#   round_up(custom_image_size, 4 KiB)) was validated on infiniti and IS
#   the intended fix path for custom-recovery + normal-boot under mode-1.
#   It will be rebuilt under Cleanup Phase 2 as a fresh host script and
#   on-device companion module. The technique is preserved in orphan history
#   on origin at feature/synthesize-fastboot-cmd commit b26686e (retained
#   until Cleanup Phase 4 disposes of the orphan branch).
#
# This test asserts none of those three command literals reappear in
# code or scripts. Docs, memory notes, and .re-notes/ retain the names
# as historical references — those locations are excluded.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

NEEDLES=(synthesize-and-flash graft-from-staged fix-vbmeta-footer)

# Search scope: code-bearing directories. Exclude docs, memory, .re-notes,
# the test itself, and the gitignored top-level images/ tree.
SCOPES=(
  "$ROOT/GblChainloadPkg"
  "$ROOT/edk2/QcomModulePkg/Library/FastbootLib"
  "$ROOT/scripts"
  "$ROOT/tools"
)

fail=0
for needle in "${NEEDLES[@]}"; do
  hits=""
  for scope in "${SCOPES[@]}"; do
    [ -d "$scope" ] || continue
    found=$(rg -n --no-heading -- "$needle" "$scope" 2>/dev/null || true)
    if [ -n "$found" ]; then
      hits="${hits}${found}"$'\n'
    fi
  done
  if [ -n "$hits" ]; then
    echo "FAIL: '$needle' present in code paths:" >&2
    echo "$hits" >&2
    fail=1
  fi
done

if [ "$fail" -ne 0 ]; then
  exit 1
fi
echo "OK: synthesize/graft surface absent from code paths."
