#!/usr/bin/env bash
# 045_mode_taxonomy_lint.sh — assert the patch scope tables exist and
# use the expected SCOPE_* enum tags. After Task 11 there is no
# compile-time GBL_MODE gate anywhere; activation is manifest-driven at
# runtime. This lint guards the directory layout and scope-tag
# invariants the runtime relies on.
#
# PR2 Task 6: the engine moved into crates/patch-engine (Rust). The
# C source paths the lint used to anchor on no longer exist; the lint
# now anchors on the Rust crate layout instead.
set -euo pipefail

cd "$(dirname "$0")/.."

CRATE="crates/patch-engine/src"

# 1. The Rust crate exists. (Task 6 replaces all DPL C sources with the
#    crate; PatchTable.c is gone — its aggregation lives in lib.rs.)
test -f "$CRATE/lib.rs" || { echo "FAIL: missing crates/patch-engine/src/lib.rs"; exit 1; }

# 2. The retired (universal) patch lives in retired/block_efisp_recursion.rs.
#    Marked SCOPE_UNIVERSAL via PatchScope::Universal.
grep -q 'EFISP_UTF16_PATTERN' "$CRATE/retired/block_efisp_recursion.rs" \
  || { echo "FAIL: retired/block_efisp_recursion.rs missing EFISP_UTF16_PATTERN"; exit 1; }

# 3. OEM patches use PatchScope::OemOplus.
grep -q 'PatchScope::OemOplus' "$CRATE/oem/oplus/mod.rs" \
  || { echo "FAIL: oem/oplus/mod.rs must register patches under PatchScope::OemOplus"; exit 1; }

# 4. ABL-permissive patches use PatchScope::AblPermissive.
grep -q 'PatchScope::AblPermissive' "$CRATE/abl_permissive/mod.rs" \
  || { echo "FAIL: abl_permissive/mod.rs must register patches under PatchScope::AblPermissive"; exit 1; }

# 5. patch10 (libavb force-AVB-success) lives in abl_permissive/libavb_force_success.rs;
#    patch6 (lock-state fastboot-gate) lives in abl_permissive/fastboot_lock_gates.rs.
test -f "$CRATE/abl_permissive/libavb_force_success.rs" \
  || { echo "FAIL: patch10 must be in abl_permissive/libavb_force_success.rs"; exit 1; }
test -f "$CRATE/abl_permissive/fastboot_lock_gates.rs" \
  || { echo "FAIL: patch6 must be in abl_permissive/fastboot_lock_gates.rs"; exit 1; }
grep -q 'patch10-libavb-force-avb-success' "$CRATE/abl_permissive/mod.rs" \
  || { echo "FAIL: patch10 name must appear in abl_permissive/mod.rs"; exit 1; }
grep -q 'patch6-lock-state-fastboot-gate' "$CRATE/abl_permissive/mod.rs" \
  || { echo "FAIL: patch6 name must appear in abl_permissive/mod.rs"; exit 1; }
if grep -rq 'patch9-avb-locked-recoverable-continue' "$CRATE/abl_permissive/"; then
  echo "FAIL: patch9 is superseded by patch10; remove patch9 from abl_permissive/"
  exit 1
fi

# 6. patch7 is OEM scope, lives in oem/oplus/bypass_warning.rs.
test -f "$CRATE/oem/oplus/bypass_warning.rs" \
  || { echo "FAIL: patch7 must be in oem/oplus/bypass_warning.rs"; exit 1; }
grep -q 'patch7-orange-screen' "$CRATE/oem/oplus/mod.rs" \
  || { echo "FAIL: patch7 must be registered in oem/oplus/mod.rs"; exit 1; }

# 7. Host-only modules are gated behind `feature = "host"` so the
#    firmware staticlib doesn't drag OEM / retired code on-device.
grep -q '#\[cfg(feature = "host")\]' "$CRATE/lib.rs" \
  || { echo "FAIL: host-only modules must be gated behind feature = \"host\""; exit 1; }

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
    GblChainloadPkg scripts tests crates \
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
