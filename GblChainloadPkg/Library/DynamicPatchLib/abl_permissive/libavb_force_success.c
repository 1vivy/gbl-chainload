/** @file libavb_force_success.c — patch10: libavb force-AVB-success.

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
    See docs/project/re-findings.md.

    Replaces patch9 v2 (deleted).  Scope: ABL-permissive (formerly mode-1).
**/

#include "../../../Include/Library/PatchDesc.h"
#include "../Internal/ScanLib.h"
#include "../Internal/Encode.h"
#include "../Internal/Arm64Decode.h"
#include "Signatures.h"

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

CONST PATCH_DESC kAblPermissiveLibavbPatches[] = {
  {
    .Name      = "patch10-libavb-force-avb-success",
    .Scope     = SCOPE_MODE_1,
    .Mandatory = TRUE,
    .Apply     = ApplyAvbForceSuccess,
  },
};
CONST UINTN kAblPermissiveLibavbPatchesCount =
  sizeof (kAblPermissiveLibavbPatches) / sizeof (kAblPermissiveLibavbPatches[0]);
