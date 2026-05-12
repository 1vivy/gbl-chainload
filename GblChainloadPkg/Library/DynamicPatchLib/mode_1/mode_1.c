/** @file mode_1.c — mode-1-scope patches (only included when GBL_MODE>=1).

    patch10 — libavb force-AVB-success (replaces patch9 v2).
      Anchor on the unique libavb string `"Persistent values required for
      AVB_HASHTREE_ERROR_MODE_MANAGED_RESTART_AND_EIO"` (verbatim AOSP source
      text in `avb_slot_verify.c`). From the string xref, locate the
      enclosing function entry via PACIASP backscan, then apply two
      rewrites inside `avb_slot_verify`:
        10a — entry-prologue `mov wN, w3` → `orr wN, w3, #1` (any Rd)
              Forces bit 0 of `flags` (AVB_SLOT_VERIFY_FLAGS_ALLOW_VERIFICATION_ERROR)
              high in the saved-flags register. All ~7 downstream
              `!allow_verification_error` gates inside libavb pass through
              without bailing, so SlotData is fully populated for every
              recoverable error class.
        10c — exit return-value `mov w0, wM` → `mov w0, #0` (any Rm)
              Forces the function to return AVB_SLOT_VERIFY_RESULT_OK
              regardless of internal `ret` state. ABL/QcomModulePkg sees
              clean OK + populated SlotData and takes the success branch.
      Subsumes patch9's Site V (caller-side AVE force) AND Site G/C
      (caller-side post-call gate skip) into one libavb-internal patch.
      See docs/re/patch10-libavb-force-success.md.

    patch6 — lock-state fastboot-gate.  Mode-1 fakelocks the VerifiedBoot
    view; ABL's in-fastboot command dispatcher then refuses flash / erase /
    slot-change / snapshot-cancel.  For each of those four refusal strings,
    locate the ADRP+ADD pair in .text that loads the string pointer and
    rewrite the preceding gate:
      Pattern A — `CBZ Wn, L_error` jumps INTO the error block.  NOP the CBZ.
      Pattern B — `B.NE skip_error` jumps PAST the error block.  Rewrite to
                   unconditional `B skip_error` with the same target.
    See docs/re/abl-lock-state-fastboot-gate.md for the RE pass. **/

#include "../../../Include/Library/PatchDesc.h"
#include "../Internal/ScanLib.h"
#include "../Internal/Encode.h"
#include "../Internal/Arm64Decode.h"
#include "Signatures.h"

/* ---- patch10: libavb force-AVB-success ---------------------------------- */

STATIC PATCH_OUTCOME
ApplyAvbForceSuccess (
  IN OUT UINT8  *Buf,
  IN     UINT32  Size
  )
{
  UINT32      StrOff, AdrpOff, FuncEntry;
  UINT32      MovFromW3Off = 0, MovToW0Off = 0, RetOff = 0;
  UINT32      Word, Rd, OrrInsn;
  UINTN       Probe;
  SCAN_RESULT R;

  /* 1. Find the unique libavb string. If absent, this PE doesn't carry the
        libavb avb_slot_verify path — report MISS, mode-1 isn't shipped here. */
  R = ScanFor (Buf, Size, (CONST UINT8 *)kPatch10AnchorStr, NULL,
               sizeof (kPatch10AnchorStr) - 1, &StrOff);
  if (R != SCAN_FOUND) {
    return (R == SCAN_AMBIGUOUS) ? PATCH_AMBIGUOUS : PATCH_MISS;
  }

  /* 2. Find the ADRP+ADD pair in .text that loads the string pointer. The
        pair lives inside `avb_slot_verify`'s body. */
  R = Arm64FindAdrpAddTargeting (Buf, Size, StrOff, /*RestrictToExec=*/TRUE,
                                 &AdrpOff);
  if (R != SCAN_FOUND) {
    return (R == SCAN_AMBIGUOUS) ? PATCH_AMBIGUOUS : PATCH_MISS;
  }

  /* 3. Walk backward from the ADRP to find the nearest PACIASP (function
        entry on AArch64 PAC-equipped builds). Both observed fixtures
        (infiniti, myron) emit PACIASP at avb_slot_verify entry. */
  FuncEntry = 0;
  for (Probe = AdrpOff; Probe >= 4; Probe -= 4) {
    if (ReadInstrU32 (Buf, (UINT32)(Probe - 4)) == kArm64PaciaspWord) {
      FuncEntry = (UINT32)(Probe - 4);
      break;
    }
  }
  if (FuncEntry == 0) {
    return PATCH_MISS;
  }

  /* 4. From the function entry, scan forward up to ~30 instructions for
        the prologue `mov wN, w3` (the compiler's stash of the `flags` arg
        into a callee-saved register). */
  for (Probe = FuncEntry; Probe + 4 <= FuncEntry + (30U * 4U) && Probe + 4 <= Size; Probe += 4) {
    Word = ReadInstrU32 (Buf, (UINT32)Probe);
    if ((Word & kArm64MovFromW3Mask) == kArm64MovFromW3Pat) {
      MovFromW3Off = (UINT32)Probe;
      break;
    }
  }
  if (MovFromW3Off == 0) {
    return PATCH_MISS;
  }

  /* 5. From the function entry, scan forward until the first `ret`. That's
        the function's exit point. */
  for (Probe = FuncEntry; Probe + 4 <= Size; Probe += 4) {
    if (ReadInstrU32 (Buf, (UINT32)Probe) == kArm64RetWord) {
      RetOff = (UINT32)Probe;
      break;
    }
  }
  if (RetOff == 0) {
    return PATCH_MISS;
  }

  /* 6. Walk backward from the ret (skip over the stack-teardown + AUTIASP
        pair) looking for the final `mov w0, wM` — the return-value
        materialization. Limit scan window so we don't reach into the
        prologue. */
  for (Probe = RetOff; Probe > FuncEntry && Probe + 4 > RetOff - 0x40U; Probe -= 4) {
    Word = ReadInstrU32 (Buf, (UINT32)(Probe - 4));
    if ((Word & kArm64MovToW0Mask) == kArm64MovToW0Pat) {
      MovToW0Off = (UINT32)(Probe - 4);
      break;
    }
  }
  if (MovToW0Off == 0) {
    return PATCH_MISS;
  }

  /* 7. Apply both rewrites.
        10a: preserve Rd from the original mov-from-w3, encode as
             orr wRd, w3, #1.
        10c: write `mov w0, #0` unconditionally. */
  Rd      = ReadInstrU32 (Buf, MovFromW3Off) & 0x1FU;
  OrrInsn = kArm64OrrW3OneBase | Rd;
  WriteInstrU32 (Buf, MovFromW3Off, OrrInsn);
  WriteInstrU32 (Buf, MovToW0Off,   kArm64MovW0Zero);

  return PATCH_OK;
}

/* ---- patch6: lock-state fastboot-gate ----------------------------------- */

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
     patch. Mode-1 is OnePlus/Oppo-targeted; report MISS so the engine
     records the absent coverage instead of silently claiming OK. */
  if (Found == 0) {
    return PATCH_MISS;
  }
  return PATCH_OK;
}

CONST PATCH_DESC kMode1Patches[] = {
  {
    .Name      = "patch10-libavb-force-avb-success",
    .Scope     = SCOPE_MODE_1,
    .Mandatory = TRUE,
    .Apply     = ApplyAvbForceSuccess,
  },
  {
    .Name      = "patch6-lock-state-fastboot-gate",
    .Scope     = SCOPE_MODE_1,
    .Mandatory = TRUE,
    .Apply     = ApplyLockStateFastbootGate,
  },
};
CONST UINTN kMode1PatchesCount =
  sizeof (kMode1Patches) / sizeof (kMode1Patches[0]);
