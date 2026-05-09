/** @file oneplus_canoe.c — OnePlus/Oppo (Phoenix/canoe) family OEM patches.

  ## Patch 7 — orange-state-screen + unlock-warning + 5-second boot-delay gate

  LinuxLoaderEntry guards an orange-state warning block with a CBZ that skips
  the block when the device is locked.  Rewriting that CBZ as an unconditional B
  always skips the block, regardless of lock state.

  Anchor: the 4 CSEL/AND bytes at the equivalent of infiniti:0x78EC, which
  immediately precede the CBZ.  The anchor is unique in the executable section
  and does not include the CBZ word itself, so patching is idempotent.

  Faithful port of gbl_root_canoe tools/patchlib.h:patch_orange_state_screen.
  Non-mandatory — cosmetic only.
**/

#include "../../../Include/Library/PatchDesc.h"
#include "../Internal/ScanLib.h"
#include "../Internal/Encode.h"
#include "Signatures.h"

STATIC PATCH_OUTCOME
ApplyOrangeScreen (
  IN OUT UINT8  *Buf,
  IN     UINT32  Size
  )
{
  UINT32      AnchorOff;
  SCAN_RESULT R;

  R = ScanForBoundedSection (Buf, Size, /*ExecOnly=*/TRUE,
                             kPatch7AnchorPattern, NULL,
                             kPatch7AnchorPatternLen, &AnchorOff);
  if (R == SCAN_NOT_FOUND) return PATCH_MISS;
  if (R == SCAN_AMBIGUOUS)  return PATCH_AMBIGUOUS;
  if (R != SCAN_FOUND)      return PATCH_MISS;

  WriteInstrU32 (Buf, AnchorOff + kPatch7RewriteDelta, kPatch7BUnconditionalInsn);
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
