#!/usr/bin/env bash
# 048_hook_lifecycle_lint.sh — assert hook install failure paths can roll back
# this image's protocol vtable patches without stripping hooks from a successful
# chainloaded ABL handoff.
set -euo pipefail
cd "$(dirname "$0")/.."

PHL="GblChainloadPkg/Library/ProtocolHookLib"
BOOT="GblChainloadPkg/Application/GblChainload/BootFlow.c"
PUB="GblChainloadPkg/Include/Library/ProtocolHookLib.h"

grep -q 'ProtocolHook_UninstallAll' "$PUB" \
  || { echo "FAIL: public ProtocolHook_UninstallAll declaration missing"; exit 1; }

grep -q 'ProtocolHook_UninstallAll' "$PHL/InstallAll.c" \
  || { echo "FAIL: InstallAll.c missing rollback helper"; exit 1; }
grep -q 'AbortInstall' "$PHL/InstallAll.c" \
  || { echo "FAIL: InstallAll.c must roll back required install failures"; exit 1; }
grep -q 'mGblHookRegistryGuid' "$PHL/InstallAll.c" \
  || { echo "FAIL: InstallAll.c missing same-session hook registry"; exit 1; }
grep -q 'existing hook mode' "$PHL/InstallAll.c" \
  || { echo "FAIL: InstallAll.c must reject cross-mode hook registry reuse"; exit 1; }
grep -q 'hook registry install failed' "$PHL/InstallAll.c" \
  || { echo "FAIL: InstallAll.c must fail closed if registry install fails"; exit 1; }
grep -q 'reusing active hooks' "$PHL/InstallAll.c" \
  || { echo "FAIL: InstallAll.c must detect and reuse active registry hooks"; exit 1; }
grep -q 'registry cleared' "$PHL/InstallAll.c" \
  || { echo "FAIL: InstallAll.c must clear registry when uninstall defers"; exit 1; }

for hook in VerifiedBoot Scm Qseecom Spss BlockIo; do
  grep -q "Uninstall${hook}Hook" "$PHL/HookCommon.h" \
    || { echo "FAIL: HookCommon.h missing Uninstall${hook}Hook"; exit 1; }
done

grep -q 'uninstall deferred' "$PHL/VerifiedBootHook.c" \
  || { echo "FAIL: VerifiedBootHook must preserve state if outer-wrapped"; exit 1; }
grep -q 'uninstall deferred' "$PHL/ScmHook.c" \
  || { echo "FAIL: ScmHook must preserve state if outer-wrapped"; exit 1; }
grep -q 'uninstall deferred' "$PHL/QseecomHook.c" \
  || { echo "FAIL: QseecomHook must preserve state if outer-wrapped"; exit 1; }
grep -q 'uninstall deferred' "$PHL/BlockIoHook.c" \
  || { echo "FAIL: BlockIoHook must preserve state if outer-wrapped"; exit 1; }

grep -q 'ProtocolHook_UninstallAll ();' "$BOOT" \
  || { echo "FAIL: BootFlow.c must clean up if LoadImage/StartImage returns"; exit 1; }

# Do not unhook before the normal StartImage handoff; ABL FastbootLib remains a
# valid hooked diagnostic surface.
handoff_line=$(grep -n 'handing off to patched ABL' "$BOOT" | cut -d: -f1 | head -n1)
start_line=$(grep -n 'StartImage (ImageHandle' "$BOOT" | cut -d: -f1 | head -n1)
unhook_before_start=$(awk -v a="$handoff_line" -v b="$start_line" \
  'NR >= a && NR <= b && /ProtocolHook_UninstallAll/ { print NR }' "$BOOT")
if [[ -n "$unhook_before_start" ]]; then
  echo "FAIL: hooks must stay installed for chainloaded ABL handoff"; exit 1
fi

echo "ok 048_hook_lifecycle_lint"
