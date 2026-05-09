/** @file mode_1.c — mode-1-scope patches (only included when GBL_MODE==1).

  Patch 9 — avb-locked-recoverable-continue

  Splits libavb's allow_verification_error from the AVB-result-to-bootstate
  decision so libavb returns populated SlotData for recoverable failures while
  w29 (downstream lock-state report) stays false.

  Three rewrite sites located via anchor scan (no fixed PE-size gate):
    A. VerifyFlagsInit: CSET W24,NE  -> MOV W24,#1
    B. RecoveryGate:    CBZ W29,T1   -> CBZ W24,T1'
    C. CommonGate:      CBZ W29,T2   -> CBZ W24,T2'

  Targets T1' and T2' are derived from anchor-relative offsets.  Anchors are
  32 bytes of pre-context that do not include the rewrite site, so patching
  is idempotent.
**/

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
  UINT32      AOff, BOff, COff;
  UINT32      ASite, BSite, CSite;
  UINT32      BTarget, CTarget;
  SCAN_RESULT R;

  R = ScanForBoundedSection (Buf, Size, /*ExecOnly=*/TRUE,
                             kPatch9AnchorA, NULL,
                             kPatch9AnchorALen, &AOff);
  if (R == SCAN_AMBIGUOUS) return PATCH_AMBIGUOUS;
  if (R != SCAN_FOUND)     return PATCH_MISS;

  R = ScanForBoundedSection (Buf, Size, /*ExecOnly=*/TRUE,
                             kPatch9AnchorB, NULL,
                             kPatch9AnchorBLen, &BOff);
  if (R == SCAN_AMBIGUOUS) return PATCH_AMBIGUOUS;
  if (R != SCAN_FOUND)     return PATCH_MISS;

  R = ScanForBoundedSection (Buf, Size, /*ExecOnly=*/TRUE,
                             kPatch9AnchorC, NULL,
                             kPatch9AnchorCLen, &COff);
  if (R == SCAN_AMBIGUOUS) return PATCH_AMBIGUOUS;
  if (R != SCAN_FOUND)     return PATCH_MISS;

  ASite = AOff + kPatch9SiteARewriteDelta;
  BSite = BOff + kPatch9SiteBRewriteDelta;
  CSite = COff + kPatch9SiteCRewriteDelta;

  /* Sanity-check original instructions — anchor matched but instruction word
     differs means the binary compiled from different source, not safe to patch. */
  if (ReadInstrU32 (Buf, ASite) != kPatch9SiteAOriginalInsn) return PATCH_MISS;
  if (ReadInstrU32 (Buf, BSite) != kPatch9SiteBOriginalInsn) return PATCH_MISS;
  if (ReadInstrU32 (Buf, CSite) != kPatch9SiteCOriginalInsn) return PATCH_MISS;

  /* Site A: CSET W24,NE -> MOV W24,#1 */
  WriteInstrU32 (Buf, ASite, kPatch9SiteAReplacementInsn);

  /* Site B: CBZ W29,<old_T1> -> CBZ W24,<BSite + 0x328> */
  BTarget = BSite + kPatch9SiteBTargetDelta;
  if (!RewriteCbz (Buf, BSite, /*Reg=*/24, BTarget)) return PATCH_MISS;

  /* Site C: CBZ W29,<old_T2> -> CBZ W24,<CSite + 0x1B4> */
  CTarget = CSite + kPatch9SiteCTargetDelta;
  if (!RewriteCbz (Buf, CSite, /*Reg=*/24, CTarget)) return PATCH_MISS;

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
