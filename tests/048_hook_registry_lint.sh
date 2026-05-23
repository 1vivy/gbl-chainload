#!/usr/bin/env bash
# 048_hook_registry_lint.sh — assert same-session hook detection stays small
# and does not grow into protocol-slot uninstall machinery.
set -euo pipefail
cd "$(dirname "$0")/.."

PHL="GblChainloadPkg/Library/ProtocolHookLib"

grep -q 'mGblHookRegistryGuid' "$PHL/InstallAll.c" \
  || { echo "FAIL: missing hook registry GUID"; exit 1; }
grep -q 'CheckNoExistingHooks' "$PHL/InstallAll.c" \
  || { echo "FAIL: missing existing-hook registry check"; exit 1; }
grep -q 'existing hook mode' "$PHL/InstallAll.c" \
  || { echo "FAIL: cross-mode registry reuse must fail closed"; exit 1; }
grep -q 'hard reset before re-running boot-efi' "$PHL/InstallAll.c" \
  || { echo "FAIL: same-mode rerun warning must name hard reset recovery"; exit 1; }
grep -q 'hook registry install failed' "$PHL/InstallAll.c" \
  || { echo "FAIL: registry install failure must fail closed before hooks"; exit 1; }

if grep -R -n 'ProtocolHook_UninstallAll\|Uninstall.*Hook' \
  GblChainloadPkg/Include/Library/ProtocolHookLib.h "$PHL"; then
  echo "FAIL: detect-only registry PR must not add hook uninstall machinery"; exit 1
fi

echo "ok 048_hook_registry_lint"
