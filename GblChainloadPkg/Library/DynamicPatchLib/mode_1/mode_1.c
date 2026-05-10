/** @file mode_1.c — mode-1-scope patches (only included when GBL_MODE>=1).

    patch9 v2 — Approach A.  Two-site rewrite:
      Site V — force VerifyFlags's ALLOW_VERIFICATION_ERROR bit (cset → mov w24,#1).
      Site G — bypass the post-libavb AllowVerificationError gate (cbz → nop).

    See docs/re/patch9-v2-disassembly.md for the data backing the anchors. **/

#include "../../../Include/Library/PatchDesc.h"
#include "../Internal/ScanLib.h"
#include "../Internal/Encode.h"
#include "Signatures.h"

STATIC PATCH_OUTCOME
ApplyAvbLockedRecoverableContinue (
  IN OUT UINT8  *Buf,
  IN     UINT32  Size
  )
{
  UINT32      VOff, GOff;
  SCAN_RESULT R;

  /* Site V: locate VerifyFlags-derivation cset, rewrite to mov w24,#1. */
  R = ScanForBoundedSection (Buf, Size, /*ExecOnly=*/TRUE,
                             kPatch9SiteVAnchor, NULL,
                             kPatch9SiteVAnchorLen, &VOff);
  if (R != SCAN_FOUND) {
    return (R == SCAN_AMBIGUOUS) ? PATCH_AMBIGUOUS : PATCH_MISS;
  }

  /* Site G: locate post-libavb cbz, rewrite to nop. */
  R = ScanForBoundedSection (Buf, Size, /*ExecOnly=*/TRUE,
                             kPatch9SiteGAnchor, NULL,
                             kPatch9SiteGAnchorLen, &GOff);
  if (R != SCAN_FOUND) {
    return (R == SCAN_AMBIGUOUS) ? PATCH_AMBIGUOUS : PATCH_MISS;
  }

  WriteInstrU32 (Buf, VOff + kPatch9SiteVRewriteDelta, kPatch9SiteVReplacement);
  WriteInstrU32 (Buf, GOff + kPatch9SiteGRewriteDelta, kPatch9SiteGReplacement);
  return PATCH_OK;
}

CONST PATCH_DESC kMode1Patches[] = {
  {
    .Name      = "patch9-avb-locked-recoverable-continue",
    .Scope     = SCOPE_MODE_1,
    .Mandatory = TRUE,
    .Apply     = ApplyAvbLockedRecoverableContinue,
  },
};
CONST UINTN kMode1PatchesCount =
  sizeof (kMode1Patches) / sizeof (kMode1Patches[0]);
