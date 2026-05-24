#!/usr/bin/env bash
# 045_mode_taxonomy_lint.sh — assert the patch scope tables exist and use the
# expected SCOPE_* enum tags. After Task 11 there is no compile-time GBL_MODE
# gate anywhere; activation is manifest-driven at runtime. This lint guards
# the directory layout and scope-tag invariants the runtime relies on.
set -euo pipefail

cd "$(dirname "$0")/.."

PKG="GblChainloadPkg/Library/DynamicPatchLib"

# 1. PatchTable.c exists. (Task 5 restructure: the firmware-side mode-1 gate
#    was moved out; abl_permissive/* now compiles unconditionally and the OEM
#    file is host-only via #ifdef __HOST_BUILD__. Task 6 renamed the scope
#    enum to SCOPE_ABL_PERMISSIVE / SCOPE_OEM_OPLUS.)
test -f "$PKG/PatchTable.c" || { echo "FAIL: missing PatchTable.c"; exit 1; }

# 2. Universal (retired) patches use SCOPE_UNIVERSAL.
grep -q 'SCOPE_UNIVERSAL' "$PKG/retired/block_efisp_recursion.c" \
  || { echo "FAIL: retired/block_efisp_recursion.c must declare SCOPE_UNIVERSAL"; exit 1; }

# 3. OEM patches use SCOPE_OEM_OPLUS.
grep -q 'SCOPE_OEM_OPLUS' "$PKG/oem/oplus/bypass_warning.c" \
  || { echo "FAIL: oem/oplus/bypass_warning.c must declare SCOPE_OEM_OPLUS"; exit 1; }

# 4. ABL-permissive patches use SCOPE_ABL_PERMISSIVE.
grep -q 'SCOPE_ABL_PERMISSIVE' "$PKG/abl_permissive/libavb_force_success.c" \
  || { echo "FAIL: abl_permissive/libavb_force_success.c must declare SCOPE_ABL_PERMISSIVE"; exit 1; }
grep -q 'SCOPE_ABL_PERMISSIVE' "$PKG/abl_permissive/fastboot_lock_gates.c" \
  || { echo "FAIL: abl_permissive/fastboot_lock_gates.c must declare SCOPE_ABL_PERMISSIVE"; exit 1; }

# 5. ABL-permissive patches live under abl_permissive/, NOT in retired/ or oem/.
#    patch10 (libavb force-AVB-success) lives in libavb_force_success.c;
#    patch6 (lock-state fastboot-gate) lives in fastboot_lock_gates.c.
if grep -q 'patch10-libavb-force-avb-success' "$PKG/retired/block_efisp_recursion.c" \
   || grep -q 'patch10-libavb-force-avb-success' "$PKG/oem/oplus/bypass_warning.c"; then
  echo "FAIL: patch10 must be in abl_permissive/, not retired/ or oem/"; exit 1
fi
grep -q 'patch10-libavb-force-avb-success' "$PKG/abl_permissive/libavb_force_success.c" \
  || { echo "FAIL: patch10 must be in abl_permissive/libavb_force_success.c"; exit 1; }
if grep -q 'patch6-lock-state-fastboot-gate' "$PKG/retired/block_efisp_recursion.c" \
   || grep -q 'patch6-lock-state-fastboot-gate' "$PKG/oem/oplus/bypass_warning.c"; then
  echo "FAIL: patch6 must be in abl_permissive/, not retired/ or oem/"; exit 1
fi
grep -q 'patch6-lock-state-fastboot-gate' "$PKG/abl_permissive/fastboot_lock_gates.c" \
  || { echo "FAIL: patch6 must be in abl_permissive/fastboot_lock_gates.c"; exit 1; }
if grep -rq 'patch9-avb-locked-recoverable-continue' "$PKG/abl_permissive/"; then
  echo "FAIL: patch9 is superseded by patch10; remove patch9 from abl_permissive/"
  exit 1
fi

# 6. patch1 lives in retired/ (universal scope).  Task 10 drops it entirely.
grep -q 'patch1-efisp-recursion' "$PKG/retired/block_efisp_recursion.c" \
  || { echo "FAIL: patch1 must be in retired/block_efisp_recursion.c"; exit 1; }

# 7. patch7 is oem scope.
grep -q 'patch7-orange-screen' "$PKG/oem/oplus/bypass_warning.c" \
  || { echo "FAIL: patch7 must be in oem/oplus/bypass_warning.c"; exit 1; }

# 8. Universal preservation is narrow: TZ soft-fuse drop plus reserve writes.
grep -q 'UniversalPolicy_ShouldDropScmSip' \
  GblChainloadPkg/Library/ProtocolHookLib/ScmHook.c \
  || { echo "FAIL: ScmHook missing universal SIP drop"; exit 1; }
grep -q 'IsOplusReserve1' \
  GblChainloadPkg/Library/ProtocolHookLib/BlockIoHook.c \
  || { echo "FAIL: BlockIoHook missing reserve partition classification"; exit 1; }
grep -q 'op=write-swallow' \
  GblChainloadPkg/Library/ProtocolHookLib/BlockIoHook.c \
  || { echo "FAIL: BlockIoHook missing reserve write swallow"; exit 1; }

# VB/OplusSec persistence suppression is mode-1 overlay, not universal mode-0 policy.
grep -q 'FakelockOverlay_OnVbWriteConfig' \
  GblChainloadPkg/Library/ProtocolHookLib/VerifiedBootHook.c \
  || { echo "FAIL: VerifiedBootHook missing mode-1 VB write swallow"; exit 1; }
grep -q 'FakelockOverlay_OnVbReset' \
  GblChainloadPkg/Library/ProtocolHookLib/VerifiedBootHook.c \
  || { echo "FAIL: VerifiedBootHook missing mode-1 VB reset swallow"; exit 1; }
grep -q 'FakelockOverlay_ShouldDropQseeOplusSec' \
  GblChainloadPkg/Library/ProtocolHookLib/QseecomHook.c \
  || { echo "FAIL: QseecomHook missing mode-1 OplusSec drop"; exit 1; }

# 9. ProtocolHook_InstallAll exists.
grep -q 'ProtocolHook_InstallAll' \
  GblChainloadPkg/Library/ProtocolHookLib/InstallAll.c \
  || { echo "FAIL: InstallAll.c missing main entry"; exit 1; }
test -f GblChainloadPkg/Library/ProtocolHookLib/ProtocolHookLib.inf \
  || { echo "FAIL: missing ProtocolHookLib.inf"; exit 1; }
test -f GblChainloadPkg/Include/Library/ProtocolHookLib.h \
  || { echo "FAIL: missing public ProtocolHookLib.h"; exit 1; }

# 10. Mode-3 is dropped from user-facing mode taxonomy. Task 11 also dropped
# GBL_MODE entirely, so an active `GBL_MODE == 3` reference would itself be
# a regression — keep the lint anchored to the mode-3 string forms too.
# Scoped to gbl-chainload-controlled surfaces so unrelated upstream EDK2
# "mode 3" text does not trip the lint.
if grep -RnE --exclude=045_mode_taxonomy_lint.sh \
    'GBL_MODE[[:space:]]*==[[:space:]]*3|mode-3|SCOPE_MODE_3' \
    GblChainloadPkg scripts tests \
    edk2/QcomModulePkg/Library/FastbootLib \
    edk2/QcomModulePkg/Library/BootLib 2>/dev/null; then
  echo "FAIL: mode-3 must not be advertised or gated in active surfaces"
  exit 1
fi

# 11. Task 11 collapse: no -DGBL_MODE=, no DEFINE GBL_MODE, no $(GBL_MODE),
#     no env GBL_MODE, no -D GBL_MODE in the build descriptor or scripts.
#     The literal token "GBL_MODE" is allowed in comments and in unrelated
#     include guards (GBL_MODE2_PROFILE_PARSE_H_), so be specific about the
#     forms that would actually re-enable a per-mode compile.
if grep -RnE -- '-D[[:space:]]*GBL_MODE[=[:space:]]|DEFINE[[:space:]]+GBL_MODE[[:space:]]|\$\(GBL_MODE\)|^[[:space:]]*GBL_MODE=|^[[:space:]]*export[[:space:]]+GBL_MODE\b|-e[[:space:]]+GBL_MODE=' \
    GblChainloadPkg/GblChainloadPkg.dsc \
    GblChainloadPkg/Application \
    GblChainloadPkg/Library \
    scripts/build.sh scripts/build-inside-docker.sh 2>/dev/null; then
  echo "FAIL: GBL_MODE residue in build system — Task 11 collapse incomplete"
  exit 1
fi

echo "ok 045_mode_taxonomy_lint"
