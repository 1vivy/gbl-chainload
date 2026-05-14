#!/usr/bin/env bash
# 046_mode1_protocol_hook_lint.sh — assert mode-1 overlay paths exist and the
# legacy FAKELOCKED gating has been replaced by GBL_MODE==1 conditional or
# Mode1Policy_* calls.
set -euo pipefail
cd "$(dirname "$0")/.."

PHL="GblChainloadPkg/Library/ProtocolHookLib"

# Mode1Overlay sources exist.
test -f "$PHL/Mode1Overlay.c" || { echo "FAIL: missing Mode1Overlay.c"; exit 1; }
test -f "$PHL/Mode1Overlay.h" || { echo "FAIL: missing Mode1Overlay.h"; exit 1; }

# VerifiedBootHook calls Mode1Policy_* functions.
grep -q 'Mode1Policy_OnVbReadConfig_Post' "$PHL/VerifiedBootHook.c" \
  || { echo "FAIL: VerifiedBootHook missing Mode1Policy_OnVbReadConfig_Post call"; exit 1; }
grep -q 'Mode1Policy_OnVbDeviceInit_PrePost' "$PHL/VerifiedBootHook.c" \
  || { echo "FAIL: VerifiedBootHook missing Mode1Policy_OnVbDeviceInit_PrePost call"; exit 1; }

# Mode1 policy functions are gated behind GBL_MODE==1.
grep -q '#if (GBL_MODE == 1)' "$PHL/Mode1Overlay.c" \
  || { echo "FAIL: Mode1Overlay.c must gate body behind GBL_MODE==1"; exit 1; }

# No legacy FAKELOCKED / MODE_DEBUG / AUTO_DEBUG_MODE references in slot wrappers.
if grep -nE 'FAKELOCKED|MODE_DEBUG|AUTO_DEBUG_MODE' "$PHL/VerifiedBootHook.c" \
   "$PHL/QseecomHook.c" "$PHL/ScmHook.c" "$PHL/SpssHook.c" 2>/dev/null; then
  echo "FAIL: legacy mode strings still present in slot wrappers"; exit 1
fi

# BootFlow.c installs protocol hooks for all modes; mode-specific policy lives
# in the hook wrappers and Mode1Policy_* calls.
grep -q 'ProtocolHook_InstallAll (&HookRes)' \
  GblChainloadPkg/Application/GblChainload/BootFlow.c \
  || { echo "FAIL: BootFlow.c must install protocol hooks"; exit 1; }

echo "ok 046_mode1_protocol_hook_lint"
