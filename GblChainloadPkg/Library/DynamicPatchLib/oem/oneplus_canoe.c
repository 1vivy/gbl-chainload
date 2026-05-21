/** @file oneplus_canoe.c — OnePlus/Oppo/Realme (oplus / canoe) family OEM patches.

  ## Patch 7 — orange-state-screen + unlock-warning + 5-second boot-delay gate

  LinuxLoaderEntry guards an orange-state warning block with a CBZ that skips
  the block when the device is locked.  Rewriting that CBZ as an unconditional B
  always skips the block, regardless of lock state.

  Anchor (see Signatures.h): a 24-byte, 6-instruction run ending in the CSEL
  at the equivalent of infiniti:0x78EC (anchor range 0x78D8-0x78EF).  The CBZ
  rewrite site sits at AnchorOff + 0x18.  The anchor is unique in the
  executable section and excludes the CBZ word itself, so patching is
  idempotent.

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
