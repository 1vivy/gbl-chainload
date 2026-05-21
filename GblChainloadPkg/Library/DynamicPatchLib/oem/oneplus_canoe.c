/** @file oneplus_canoe.c — OnePlus/Oppo/Realme (oplus / canoe) family OEM patches.

  ## Patch 7 — orange-state-screen + unlock-warning + 5-second boot-delay gate

  LinuxLoaderEntry guards an orange-state warning block with a CBZ that skips
  the block when the device is locked.  Rewriting that CBZ as an unconditional B
  always skips the block, regardless of lock state.

  String-anchored (see Signatures.h).  The orange-state warning text
  "Your device has been unlocked and can't be trusted" is invariant across OTA
  builds and is referenced by exactly one ADRP+ADD.  Resolve that pair, then
  walk backward to the nearest CBZ Wn — the lock-state guard — and rewrite it
  to an unconditional B (branch target preserved) so the warning + 5-second
  delay block is always skipped.  This is robust to the instruction-level
  shifts that broke a fixed byte anchor between EU-16.0.5.703 and IN-16.0.7.201
  (the intervening instructions are X-form CBZ/CBNZ, which the W-form scan
  steps over).

  Idempotency: after the rewrite the guard slot is a forward unconditional B;
  the backward scan treats that as already-applied and returns PATCH_OK.

  Verified on EU-16.0.5.703 (CBZ @0x78F0), IN-16.0.7.201 (@0x76D8) and
  fairlady-CN-16.0.7.200 (@0x76D8).

  Faithful port of gbl_root_canoe tools/patchlib.h:patch_orange_state_screen.
  Non-mandatory — cosmetic only; PATCH_MISS on non-matching ABLs is a clean
  no-op.

  Scope: SCOPE_OEM_ONEPLUS.  Selected at host build time by
  `abl-patcher --oem oneplus`, and aggregated automatically by the EFI
  runtime patch table (mode-1 fakelocks the orange-state code path so the
  rewrite is dead code there; mode-2 keeps ABL honest and needs the rewrite
  to silence the warning).
**/

#include "../../../Include/Library/PatchDesc.h"
#include "../Internal/ScanLib.h"
#include "../Internal/Encode.h"
#include "../Internal/Arm64Decode.h"
#include "Signatures.h"

PATCH_OUTCOME
ApplyOrangeScreen (
  IN OUT UINT8  *Buf,
  IN     UINT32  Size
  )
{
  UINT32          StrOff, AdrpOff, Tgt, RegOrCond, Word;
  ARM64_INSN_KIND Kind;
  SCAN_RESULT     R;
  UINTN           Probe, Lo;

  /* 1. Find the orange-state warning text. Absent -> this PE has no orange
        warning to silence (e.g. a non-oplus ABL): clean MISS. */
  R = ScanFor (Buf, Size, (CONST UINT8 *)kPatch7WarnStr, NULL,
               sizeof (kPatch7WarnStr) - 1, &StrOff);
  if (R != SCAN_FOUND) return (R == SCAN_AMBIGUOUS) ? PATCH_AMBIGUOUS : PATCH_MISS;

  /* 2. Resolve the unique ADRP+ADD that loads the warning-string pointer. */
  R = Arm64FindAdrpAddTargeting (Buf, Size, StrOff, /*RestrictToExec=*/TRUE,
                                 &AdrpOff);
  if (R != SCAN_FOUND) return (R == SCAN_AMBIGUOUS) ? PATCH_AMBIGUOUS : PATCH_MISS;
  if (AdrpOff < 4) return PATCH_MISS;

  /* 3. Walk backward to the lock-state guard. The intervening instructions are
        X-form CBZ/CBNZ, stepped over by the W-form match.
          - CBZ Wn               -> rewrite to an unconditional B (skip warning)
          - forward unconditional B -> already patched (idempotent) */
  Lo = (AdrpOff > kPatch7BackScanWindow) ? (AdrpOff - kPatch7BackScanWindow) : 4U;
  for (Probe = AdrpOff - 4; Probe >= Lo; Probe -= 4) {
    Word = ReadInstrU32 (Buf, (UINT32)Probe);
    if (Arm64DecodeBranch (Word, (UINT32)Probe, &Kind, &Tgt, &RegOrCond)) {
      if (Kind == ARM64_INSN_CBZ_W) {
        RewriteBUncond (Buf, (UINT32)Probe, Tgt);
        return PATCH_OK;
      }
      if (Kind == ARM64_INSN_B && Tgt > (UINT32)Probe) {
        return PATCH_OK;   /* guard already rewritten */
      }
    }
    if (Probe < 4) break;   /* guard UINTN underflow */
  }

  return PATCH_MISS;
}

CONST PATCH_DESC kOemOneplusPatches[] = {
  {
    .Name      = "patch7-orange-screen",
    .Scope     = SCOPE_OEM_ONEPLUS,
    .Mandatory = FALSE,
    .Apply     = ApplyOrangeScreen,
  },
};

CONST UINTN kOemOneplusPatchesCount =
  sizeof (kOemOneplusPatches) / sizeof (kOemOneplusPatches[0]);
