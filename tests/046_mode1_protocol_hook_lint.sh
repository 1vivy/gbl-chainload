#!/usr/bin/env bash
# 046_mode1_protocol_hook_lint.sh — assert the protocol-hook wrappers gate
# fakelock / profile-spoof policy at RUNTIME via gManifest.Want* (Task 8),
# NOT compile-time via #if (GBL_MODE == ...). Also assert the Task-7 file
# layout (FakelockOverlay.{c,h}) is in place and exposes unconditional
# public function declarations.
set -euo pipefail
cd "$(dirname "$0")/.."

PHL="GblChainloadPkg/Library/ProtocolHookLib"

# 1. FakelockOverlay sources exist (Task-7 rename from Mode1Overlay).
test -f "$PHL/FakelockOverlay.c" || { echo "FAIL: missing FakelockOverlay.c"; exit 1; }
test -f "$PHL/FakelockOverlay.h" || { echo "FAIL: missing FakelockOverlay.h"; exit 1; }

# 2. No compile-time GBL_MODE gating anywhere in ProtocolHookLib (Task-8 dropped).
#    A future regression that re-introduces `#if (GBL_MODE == ...)` in these
#    files would defeat the manifest-driven activation contract.
if grep -nE '#if[[:space:]]*\(?[[:space:]]*GBL_MODE[[:space:]]*==' \
     "$PHL"/*.c "$PHL"/*.h 2>/dev/null; then
  echo "FAIL: ProtocolHookLib must not contain #if (GBL_MODE == ...) blocks; gate at runtime via gManifest.Want*"
  exit 1
fi

# 3. FakelockOverlay.h declares the public surface unconditionally
#    (Task-8: declarations are not wrapped in #if GBL_MODE).
for sym in \
    FakelockOverlay_OnVbReadConfig_Post \
    FakelockOverlay_OnVbDeviceInit_PrePost \
    FakelockOverlay_OnVbWriteConfig \
    FakelockOverlay_OnVbReset \
    FakelockOverlay_ShouldDropQseeOplusSec; do
  grep -q "$sym" "$PHL/FakelockOverlay.h" \
    || { echo "FAIL: FakelockOverlay.h missing public declaration $sym"; exit 1; }
done

# 4. VerifiedBootHook.c gates fakelock policy on gManifest.WantFakelockHook
#    at multiple sites (read/write/reset/devinit paths). Task 8 reported 10
#    sites — require >=8 so cosmetic refactors don't trip the lint but a
#    full revert to compile-time gating would (it'd drop to 0).
VB_HITS=$(grep -c 'gManifest\.WantFakelockHook' "$PHL/VerifiedBootHook.c" || true)
if [ "${VB_HITS:-0}" -lt 8 ]; then
  echo "FAIL: VerifiedBootHook.c has only ${VB_HITS:-0} gManifest.WantFakelockHook gates; expected >=8"
  exit 1
fi

# 5. QseecomHook.c has BOTH a fakelock path (OplusSec drop) and a
#    profile-spoof path (qsee-com profile rewrite), each runtime-gated.
grep -q 'gManifest\.WantFakelockHook' "$PHL/QseecomHook.c" \
  || { echo "FAIL: QseecomHook.c missing gManifest.WantFakelockHook gate"; exit 1; }
grep -q 'gManifest\.WantProfileSpoof' "$PHL/QseecomHook.c" \
  || { echo "FAIL: QseecomHook.c missing gManifest.WantProfileSpoof gate"; exit 1; }

# 6. SpssHook.c runtime-gates its profile-spoof rewrite on WantProfileSpoof.
grep -q 'gManifest\.WantProfileSpoof' "$PHL/SpssHook.c" \
  || { echo "FAIL: SpssHook.c missing gManifest.WantProfileSpoof gate"; exit 1; }

# 7. InstallAll.c installs slots conditionally on the same manifest bits
#    (overlay attach is also manifest-driven, not GBL_MODE-driven).
grep -q 'gManifest\.WantFakelockHook' "$PHL/InstallAll.c" \
  || { echo "FAIL: InstallAll.c must consult gManifest.WantFakelockHook"; exit 1; }
grep -q 'gManifest\.WantProfileSpoof' "$PHL/InstallAll.c" \
  || { echo "FAIL: InstallAll.c must consult gManifest.WantProfileSpoof"; exit 1; }

# 8. VerifiedBootHook still routes through FakelockOverlay_* helpers
#    (the runtime gate calls into the overlay, it doesn't inline the policy).
grep -q 'FakelockOverlay_OnVbReadConfig_Post' "$PHL/VerifiedBootHook.c" \
  || { echo "FAIL: VerifiedBootHook missing FakelockOverlay_OnVbReadConfig_Post call"; exit 1; }
grep -q 'FakelockOverlay_OnVbDeviceInit_PrePost' "$PHL/VerifiedBootHook.c" \
  || { echo "FAIL: VerifiedBootHook missing FakelockOverlay_OnVbDeviceInit_PrePost call"; exit 1; }

# 9. No legacy FAKELOCKED / MODE_DEBUG / AUTO_DEBUG_MODE references in slot wrappers.
if grep -nE 'FAKELOCKED|MODE_DEBUG|AUTO_DEBUG_MODE' \
     "$PHL/VerifiedBootHook.c" "$PHL/QseecomHook.c" \
     "$PHL/ScmHook.c" "$PHL/SpssHook.c" 2>/dev/null; then
  echo "FAIL: legacy mode strings still present in slot wrappers"; exit 1
fi

# 10. BootFlow.c installs protocol hooks unconditionally; activation lives
#     in the manifest, not in the install caller.
grep -q 'ProtocolHook_InstallAll (&HookRes)' \
  GblChainloadPkg/Application/GblChainload/BootFlow.c \
  || { echo "FAIL: BootFlow.c must install protocol hooks"; exit 1; }

echo "ok 046_mode1_protocol_hook_lint"
