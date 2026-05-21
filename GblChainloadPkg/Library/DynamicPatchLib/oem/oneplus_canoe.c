/** @file oneplus_canoe.c — OnePlus/Oppo/Realme (oplus / canoe) family OEM patches.

  ## Patch 7 — orange-state-screen + unlock-warning + 5-second boot-delay gate

  LinuxLoaderEntry guards an orange-state warning block with a CBZ that skips
  the block when the device is locked.  Rewriting that CBZ as an unconditional B
  always skips the block, regardless of lock state.

  Anchor (see Signatures.h): the orange-state CBZ (0x3400046A) is byte-identical
  across OTA builds, so the anchor starts AT the CBZ and discriminates with the
  masked 5-second-delay setup that follows it (the canoe delay_anchor).  Rewrite
  site = AnchorOff (delta 0).  Verified unique in both the EU-16.0.5.703 PE
  (CBZ @0x78F0) and the IN-16.0.7.201 PE (CBZ @0x76D8); patching is idempotent
  because the rewrite leaves the masked delay-setup tail unchanged.

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
#include "Signatures.h"

PATCH_OUTCOME
ApplyOrangeScreen (
  IN OUT UINT8  *Buf,
  IN     UINT32  Size
  )
{
  UINT32      AnchorOff;
  SCAN_RESULT R;

  R = ScanForBoundedSection (Buf, Size, /*ExecOnly=*/TRUE,
                             kPatch7AnchorPattern, kPatch7AnchorMask,
                             kPatch7AnchorPatternLen, &AnchorOff);
  if (R == SCAN_NOT_FOUND) return PATCH_MISS;
  if (R == SCAN_AMBIGUOUS)  return PATCH_AMBIGUOUS;
  if (R != SCAN_FOUND)      return PATCH_MISS;

  /* The CBZ word itself is wildcarded in the anchor (uniqueness comes from the
     trailing delay-setup), so guard the rewrite site:
       - already our B    -> idempotent success, nothing to do
       - a CBZ (0x34xxxxxx) -> rewrite to the unconditional B
       - anything else    -> refuse (anchor matched an unexpected site) */
  UINT32 Site = AnchorOff + kPatch7RewriteDelta;
  UINT32 Word = ReadInstrU32 (Buf, Site);
  if (Word == kPatch7BUnconditionalInsn) return PATCH_OK;
  if ((Word & 0xFF000000U) != 0x34000000U) return PATCH_MISS;

  WriteInstrU32 (Buf, Site, kPatch7BUnconditionalInsn);
  return PATCH_OK;
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
