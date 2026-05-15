/** @file universal.c — universal-scope patches (apply to every supported PE).

  ## Patch 1 — EFISP recursion fix

  We could totally replace this patch with our new blockio hook LOL, and
  return an EFI FAILURE when ABL tries to load the "efisp" partition. But,
  a runtime hook for it would be not worth it, and the outcome is same.

  After our gbl-chainload.efi is loaded by stock ABL, it LoadImages an
  unwrapped copy of ABL from the abl partition.  That second-stage ABL,
  if it sees the "efisp" partition label in its own search, will load
  whatever is there as the next-stage GBL — i.e., us — and we would recurse
  forever (hard brick on the watchdog).

  Patch: search the in-memory PE for the UTF-16LE bytes of "efisp" and
  rewrite to "nulls".  The string is the partition label the second-stage
  ABL searches for; with the search target gone, ABL skips that step and
  proceeds to its normal boot path.

  Faithful port of gbl_root_canoe tools/patchlib.h:patch_abl_gbl.
  Optional — updated ABLs may no longer contain the GBL/EFISP loader path.
**/

#include "../../../Include/Library/PatchDesc.h"
#include "../../../Include/Library/ScanLib.h"
#include "Signatures.h"

STATIC PATCH_OUTCOME
ApplyEfispRecursion (
  IN OUT UINT8  *Buf,
  IN     UINT32  Size
  )
{
  UINT32      Off;
  SCAN_RESULT R;

  R = ScanFor (Buf, Size,
               kEfispUtf16Pattern, NULL, sizeof (kEfispUtf16Pattern), &Off);
  if (R == SCAN_NOT_FOUND)  return PATCH_MISS;
  if (R == SCAN_AMBIGUOUS)  return PATCH_AMBIGUOUS;
  if (R != SCAN_FOUND)      return PATCH_MISS;

  /* "efisp" -> "nulls", in UTF-16LE.  Each char occupies 2 bytes;
     the high byte stays 0 (already correct from the original string). */
  Buf[Off + 0] = 'n';
  Buf[Off + 2] = 'u';
  Buf[Off + 4] = 'l';
  Buf[Off + 6] = 'l';
  Buf[Off + 8] = 's';
  return PATCH_OK;
}

CONST PATCH_DESC kUniversalPatches[] = {
  {
    .Name      = "patch1-efisp-recursion",
    .Scope     = SCOPE_UNIVERSAL,
    .Mandatory = FALSE,
    .Apply     = ApplyEfispRecursion,
  },
};
CONST UINTN kUniversalPatchesCount =
  sizeof (kUniversalPatches) / sizeof (kUniversalPatches[0]);
