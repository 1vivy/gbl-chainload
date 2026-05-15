#!/usr/bin/env bash
# 047_cleanup_lint.sh — assert dead-end files / symbols never landed in v2.
set -euo pipefail
cd "$(dirname "$0")/.."
REPO_ROOT="$(pwd)"

FAIL=0
check_absent() {
  local label="$1"; shift
  for path in "$@"; do
    if [[ -e "$path" ]]; then
      echo "FAIL: $label still present at $path"; FAIL=1
    fi
  done
}

# Allow scoping pattern searches to specific dirs to avoid hitting docs / re-notes.
check_no_match_in() {
  local label="$1" pattern="$2"; shift 2
  if grep -RnE "$pattern" "$@" 2>/dev/null; then
    echo "FAIL: $label pattern '$pattern' still present"; FAIL=1
  fi
}

# 1. EBS-mutate scaffolding
check_absent "EBS-mutate sources" \
  GblChainloadPkg/Library/ProtocolHookLib/EbsMutate.c \
  GblChainloadPkg/Library/ProtocolHookLib/EbsMutate.h \
  tests/044_bootargs_rewrite_harness.sh
check_no_match_in "EBS-mutate knob" 'GBL_DEBUG_EBS_MUTATE' \
  GblChainloadPkg scripts

# 2. UDT helper
check_absent "UDT helper artifacts" \
  tests/043_update_device_tree_callsite_anchor.sh \
  docs/project/update-device-tree-callsite-helper.md
check_no_match_in "UDT helper symbols" \
  'kUpdateDtbHelperHex|ApplyUpdateDeviceTreeLogHelper|FindUpdateDeviceTreeCallsite' \
  GblChainloadPkg
check_no_match_in "UDT helper knob" 'GBL_DEBUG_UDT_HELPER' \
  GblChainloadPkg scripts

# 3. oem pull-logfs
check_absent "pull-logfs script" scripts/pull-logfs.sh
check_no_match_in "pull-logfs invocation" 'pull-logfs|pull_logfs' \
  scripts

# 4-5. Toggle-Primary-OS / shell-boot — checked at edk2 submodule
# Use precise patterns to avoid matching upstream EDK2 commits that merely
# mention "shell boot option" in an unrelated BDS context.
if cd edk2 && git log --oneline | grep -iE 'toggle.primary.os|shell-boot|get-staged-logfs'; then
  echo "FAIL: edk2 submodule still carries banned commits"; FAIL=1
fi
cd "$REPO_ROOT"

# 6. Mode sprawl knobs — must not appear in source tree (excluding docs/project)
check_no_match_in "mode sprawl knobs in app" \
  'AUTO_DEBUG_MODE|MODE_DEBUG\b|MODE_TEMPLATE|FAKELOCKED|MINIMAL\b|MODE_1\b' \
  GblChainloadPkg/Application

# 7. Debug-variant matrix
check_no_match_in "debug-variant matrix" \
  '\-\-debug-variant|GBL_DEBUG_PATCH_ONLY|GBL_DEBUG_NO_EBS|ebs-wrapper-only|ebs-fdt-probe|ebs-scan|ebs-no-bootconfig|ebs-no-close' \
  GblChainloadPkg scripts

[[ $FAIL -eq 0 ]] && echo "ok 047_cleanup_lint" || exit 1
