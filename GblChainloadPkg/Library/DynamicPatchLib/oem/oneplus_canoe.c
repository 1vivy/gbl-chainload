/** @file oneplus_canoe.c — OnePlus/Oppo/Realme (oplus / canoe) family OEM patches.

  ## Patch 7 — orange-state-screen + unlock-warning + 5-second boot-delay gate

  LinuxLoaderEntry guards an orange-state warning block with a CBZ that skips
  the block when the device is locked.  Rewriting that CBZ as an unconditional B
  always skips the block, regardless of lock state.

  Two-tier anchoring (see Signatures.h), most-reliable first:

    Primary (string): find the orange-state warning text "Your device has been
    unlocked and can't be trusted" — invariant across OTA builds — then the
    unique ADRP+ADD that loads it, then the nearest preceding CBZ Wn.  That CBZ
    is the guard; rewrite it to an unconditional B (target preserved).  Robust
    to the instruction-level shifts that broke a fixed byte anchor between
    EU-16.0.5.703 and IN-16.0.7.201.

    Fallback (instruction pattern): the canoe delay_anchor — the orange-state
    CBZ (0x3400046A, byte-identical across builds) followed by the masked
    5-second-delay setup.  The CBZ is wildcarded so a re-scan still matches
    post-rewrite; a site guard (already-B -> OK; CBZ -> rewrite; else -> MISS)
    gives idempotency and covers builds lacking the warning string.

  Verified on the EU-16.0.5.703 (CBZ @0x78F0), IN-16.0.7.201 (@0x76D8) and
  fairlady-CN-16.0.7.200 (@0x76D8) PEs.

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

/* Fallback: instruction-pattern (canoe delay_anchor) + site guard.  Used when
   the string anchor is unavailable (warning text absent/relocated or ADRP
   resolution ambiguous) AND for the idempotent re-apply (the wildcarded CBZ
   keeps matching after rewrite; the guard returns OK on an already-B site). */
STATIC
PATCH_OUTCOME
ApplyOrangeScreenFallback (
  IN OUT UINT8  *Buf,
  IN     UINT32  Size
  )
{
  UINT32      AnchorOff, Site, Word;
  SCAN_RESULT R;

  R = ScanForBoundedSection (Buf, Size, /*ExecOnly=*/TRUE,
                             kPatch7AnchorPattern, kPatch7AnchorMask,
                             kPatch7AnchorPatternLen, &AnchorOff);
  if (R == SCAN_NOT_FOUND) return PATCH_MISS;
  if (R == SCAN_AMBIGUOUS) return PATCH_AMBIGUOUS;
  if (R != SCAN_FOUND)     return PATCH_MISS;

  Site = AnchorOff + kPatch7RewriteDelta;
  Word = ReadInstrU32 (Buf, Site);
  if (Word == kPatch7BUnconditionalInsn) return PATCH_OK;       /* already patched */
  if ((Word & 0xFF000000U) != 0x34000000U) return PATCH_MISS;   /* not a CBZ */
  WriteInstrU32 (Buf, Site, kPatch7BUnconditionalInsn);
  return PATCH_OK;
}

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

  /* --- Primary: string-anchored.  The orange-state warning text is invariant
     across OTA builds, so locate it, find the unique ADRP+ADD that loads it,
     then take the nearest CBZ Wn preceding that ADRP as the guard to rewrite.
     This is robust to the instruction-level shifts that broke a fixed byte
     anchor between EU-16.0.5.703 and IN-16.0.7.201. --- */
  R = ScanFor (Buf, Size, (CONST UINT8 *)kPatch7WarnStr, NULL,
               sizeof (kPatch7WarnStr) - 1, &StrOff);
  if (R == SCAN_FOUND) {
    R = Arm64FindAdrpAddTargeting (Buf, Size, StrOff, /*RestrictToExec=*/TRUE,
                                   &AdrpOff);
    if (R == SCAN_FOUND && AdrpOff >= 4) {
      Lo = (AdrpOff > kPatch7BackScanWindow) ? (AdrpOff - kPatch7BackScanWindow) : 4U;
      for (Probe = AdrpOff - 4; Probe >= Lo; Probe -= 4) {
        Word = ReadInstrU32 (Buf, (UINT32)Probe);
        if (Arm64DecodeBranch (Word, (UINT32)Probe, &Kind, &Tgt, &RegOrCond)
            && Kind == ARM64_INSN_CBZ_W) {
          RewriteBUncond (Buf, (UINT32)Probe, Tgt);
          return PATCH_OK;
        }
        if (Probe < 4) break;   /* guard UINTN underflow */
      }
      /* No CBZ in window: most likely already patched (the guard CBZ is now an
         unconditional B).  Fall through to the fallback, whose site guard
         returns PATCH_OK on an already-B site (idempotency). */
    }
  }

  return ApplyOrangeScreenFallback (Buf, Size);
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
