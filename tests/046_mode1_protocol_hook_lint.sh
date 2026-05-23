#!/usr/bin/env bash
# 046_mode1_protocol_hook_lint.sh — assert fakelock overlay paths exist and the
# legacy FAKELOCKED gating has been replaced by GBL_MODE==1 conditional or
# FakelockOverlay_* calls.
set -euo pipefail
cd "$(dirname "$0")/.."

PHL="GblChainloadPkg/Library/ProtocolHookLib"

# FakelockOverlay sources exist.
test -f "$PHL/FakelockOverlay.c" || { echo "FAIL: missing FakelockOverlay.c"; exit 1; }
test -f "$PHL/FakelockOverlay.h" || { echo "FAIL: missing FakelockOverlay.h"; exit 1; }

# VerifiedBootHook calls FakelockOverlay_* functions.
grep -q 'FakelockOverlay_OnVbReadConfig_Post' "$PHL/VerifiedBootHook.c" \
  || { echo "FAIL: VerifiedBootHook missing FakelockOverlay_OnVbReadConfig_Post call"; exit 1; }
grep -q 'FakelockOverlay_OnVbDeviceInit_PrePost' "$PHL/VerifiedBootHook.c" \
  || { echo "FAIL: VerifiedBootHook missing FakelockOverlay_OnVbDeviceInit_PrePost call"; exit 1; }

# Fakelock policy functions are gated behind GBL_MODE==1.
grep -q '#if (GBL_MODE == 1)' "$PHL/FakelockOverlay.c" \
  || { echo "FAIL: FakelockOverlay.c must gate body behind GBL_MODE==1"; exit 1; }

# No legacy FAKELOCKED / MODE_DEBUG / AUTO_DEBUG_MODE references in slot wrappers.
if grep -nE 'FAKELOCKED|MODE_DEBUG|AUTO_DEBUG_MODE' "$PHL/VerifiedBootHook.c" \
   "$PHL/QseecomHook.c" "$PHL/ScmHook.c" "$PHL/SpssHook.c" 2>/dev/null; then
  echo "FAIL: legacy mode strings still present in slot wrappers"; exit 1
fi

# BootFlow.c installs protocol hooks for all modes; mode-specific policy lives
# in the hook wrappers and FakelockOverlay_* calls.
grep -q 'ProtocolHook_InstallAll (&HookRes)' \
  GblChainloadPkg/Application/GblChainload/BootFlow.c \
  || { echo "FAIL: BootFlow.c must install protocol hooks"; exit 1; }

echo "ok 046_mode1_protocol_hook_lint"
