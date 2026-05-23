/** @file fastboot_lock_gates.c — patch6: lock-state fastboot-gate.

    ABL-permissive fakelocks the VerifiedBoot view; ABL's in-fastboot command
    dispatcher then refuses flash / erase / slot-change / snapshot-cancel.
    For each of those four refusal strings, locate the ADRP+ADD pair in .text
    that loads the string pointer and rewrite the preceding gate:
      Pattern A — `CBZ Wn, L_error` jumps INTO the error block.  NOP the CBZ.
      Pattern B — `B.NE skip_error` jumps PAST the error block.  Rewrite to
                   unconditional `B skip_error` with the same target.
    See docs/project/re-findings.md for the RE pass.

    Scope: ABL-permissive (formerly mode-1). **/

#include "../../../Include/Library/PatchDesc.h"
#include "../Internal/ScanLib.h"
#include "../Internal/Encode.h"
#include "../Internal/Arm64Decode.h"
#include "Signatures.h"

STATIC PATCH_OUTCOME
RewriteOneLockStateGate (
  IN OUT UINT8       *Buf,
  IN     UINT32       Size,
  IN     CONST CHAR8 *Str,
  IN     UINTN        StrLen,
  OUT    BOOLEAN     *FoundOut
  )
{
  UINT32          StrOff, AdrpOff, BranchOff, PriorWord;
  UINT32          BranchTgt, RegOrCond;
  ARM64_INSN_KIND Kind;
  SCAN_RESULT     R;

  *FoundOut = FALSE;

  /* Locate the refusal string in .rodata. Must be unique. */
  R = ScanFor (Buf, Size, (CONST UINT8 *)Str, NULL, StrLen, &StrOff);
  if (R == SCAN_NOT_FOUND) {
    /* This OEM doesn't carry this string — that gate doesn't exist here. */
    return PATCH_OK;
  }
  if (R != SCAN_FOUND) {
    return (R == SCAN_AMBIGUOUS) ? PATCH_AMBIGUOUS : PATCH_MISS;
  }
  *FoundOut = TRUE;

  /* Find the ADRP+ADD pair in .text whose decoded target equals StrOff. */
  R = Arm64FindAdrpAddTargeting (Buf, Size, StrOff, /*RestrictToExec=*/TRUE,
                                 &AdrpOff);
  if (R != SCAN_FOUND) {
    return (R == SCAN_AMBIGUOUS) ? PATCH_AMBIGUOUS : PATCH_MISS;
  }

  if (AdrpOff < 4) {
    return PATCH_MISS;
  }

  /* Pattern B: B.NE at ADRP-4 skipping past the error block. */
  PriorWord = ReadInstrU32 (Buf, AdrpOff - 4);
  Arm64DecodeBranch (PriorWord, AdrpOff - 4, &Kind, &BranchTgt, &RegOrCond);
  if (Kind == ARM64_INSN_BCOND && RegOrCond == 0x1U) {
    if (!RewriteBUncond (Buf, AdrpOff - 4, BranchTgt)) {
      return PATCH_MISS;
    }
    return PATCH_OK;
  }

  /* Pattern A: an upstream CBZ/CBNZ/B.cond jumps INTO the ADRP+ADD. NOP it. */
  R = Arm64FindCondBranchTargeting (Buf, Size, AdrpOff,
                                    /*RestrictToExec=*/TRUE, &BranchOff);
  if (R != SCAN_FOUND) {
    return (R == SCAN_AMBIGUOUS) ? PATCH_AMBIGUOUS : PATCH_MISS;
  }
  WriteInstrU32 (Buf, BranchOff, 0xD503201FU);   /* NOP */
  return PATCH_OK;
}

STATIC PATCH_OUTCOME
ApplyLockStateFastbootGate (
  IN OUT UINT8  *Buf,
  IN     UINT32  Size
  )
{
  STATIC CONST struct {
    CONST CHAR8 *Str;
    UINTN        Len;
  } Gates[] = {
    { kPatch6FlashingStr,       sizeof (kPatch6FlashingStr)       - 1 },
    { kPatch6EraseStr,          sizeof (kPatch6EraseStr)          - 1 },
    { kPatch6SlotChangeStr,     sizeof (kPatch6SlotChangeStr)     - 1 },
    { kPatch6SnapshotCancelStr, sizeof (kPatch6SnapshotCancelStr) - 1 },
  };
  UINTN         i;
  UINT32        Found = 0;
  BOOLEAN       GateFound;
  PATCH_OUTCOME O;

  for (i = 0; i < sizeof (Gates) / sizeof (Gates[0]); ++i) {
    O = RewriteOneLockStateGate (Buf, Size, Gates[i].Str, Gates[i].Len,
                                 &GateFound);
    if (GateFound) {
      Found++;
    }
    if (O != PATCH_OK) {
      return O;
    }
  }

  /* No known refusal strings present — not a supported OEM ABL for this
     patch. ABL-permissive is OnePlus/Oppo-targeted; report MISS so the
     engine records the absent coverage instead of silently claiming OK. */
  if (Found == 0) {
    return PATCH_MISS;
  }
  return PATCH_OK;
}

CONST PATCH_DESC kAblPermissiveFastbootGatePatches[] = {
  {
    .Name      = "patch6-lock-state-fastboot-gate",
    .Scope     = SCOPE_MODE_1,
    .Mandatory = TRUE,
    .Apply     = ApplyLockStateFastbootGate,
  },
};
CONST UINTN kAblPermissiveFastbootGatePatchesCount =
  sizeof (kAblPermissiveFastbootGatePatches) / sizeof (kAblPermissiveFastbootGatePatches[0]);
