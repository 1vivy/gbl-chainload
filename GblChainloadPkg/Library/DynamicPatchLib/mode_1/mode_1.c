/** @file mode_1.c — mode-1-scope patches (only included when GBL_MODE>=1).

    patch9 v2 — Approach A.  Three-site rewrite:
      Site V — force VerifyFlags's ALLOW_VERIFICATION_ERROR bit (cset → mov w24,#1).
      Site G — bypass the first post-libavb AllowVerificationError gate (cbz → nop).
      Site C — bypass the second post-libavb AllowVerificationError gate (cbz → nop).

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
  UINT32      VOff, GOff, COff;
  SCAN_RESULT R;

  /* Site V: locate VerifyFlags-derivation cset, rewrite to mov w24,#1. */
  R = ScanForBoundedSection (Buf, Size, /*ExecOnly=*/TRUE,
                             kPatch9SiteVAnchor, NULL,
                             kPatch9SiteVAnchorLen, &VOff);
  if (R != SCAN_FOUND) {
    return (R == SCAN_AMBIGUOUS) ? PATCH_AMBIGUOUS : PATCH_MISS;
  }

  /* Site G: locate first post-libavb cbz on AllowVerificationError, rewrite to nop. */
  R = ScanForBoundedSection (Buf, Size, /*ExecOnly=*/TRUE,
                             kPatch9SiteGAnchor, NULL,
                             kPatch9SiteGAnchorLen, &GOff);
  if (R != SCAN_FOUND) {
    return (R == SCAN_AMBIGUOUS) ? PATCH_AMBIGUOUS : PATCH_MISS;
  }

  /* Site C: locate second post-libavb cbz on AllowVerificationError, rewrite to nop. */
  R = ScanForBoundedSection (Buf, Size, /*ExecOnly=*/TRUE,
                             kPatch9SiteCAnchor, kPatch9SiteCAnchorMask,
                             kPatch9SiteCAnchorLen, &COff);
  if (R != SCAN_FOUND) {
    return (R == SCAN_AMBIGUOUS) ? PATCH_AMBIGUOUS : PATCH_MISS;
  }

  WriteInstrU32 (Buf, VOff + kPatch9SiteVRewriteDelta, kPatch9SiteVReplacement);
  WriteInstrU32 (Buf, GOff + kPatch9SiteGRewriteDelta, kPatch9SiteGReplacement);
  WriteInstrU32 (Buf, COff + kPatch9SiteCRewriteDelta, kPatch9SiteCReplacement);
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
