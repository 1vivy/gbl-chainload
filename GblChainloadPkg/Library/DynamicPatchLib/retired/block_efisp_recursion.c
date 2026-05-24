/** @file retired/block_efisp_recursion.c — retired patch1 (EFISP recursion fix).

  RETIRED 2026-05-22 — superseded by BlockIoHook EFISP gate (Task 9).
  Reference implementation only.  The `kUniversalPatches[]` array was
  emptied in Task 10 (this commit): the active table no longer carries
  patch1-efisp-recursion, because the BlockIoHook EFISP gate refuses
  BlockIo reads/writes against the efisp partition handle at the
  protocol layer and that is now the operational guarantee against the
  second-stage-ABL recursion.  The static `ApplyEfispRecursion` function
  below is kept solely as a reference implementation; it is marked
  __attribute__((unused)) so the compiler does not warn.

  ## Patch 1 — EFISP recursion fix (historical context)

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

STATIC PATCH_OUTCOME __attribute__((unused))
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

/* RETIRED (Task 10) — patch1-efisp-recursion dropped from the active table.
   The BlockIoHook EFISP gate (Task 9) supersedes its operational role.
   A zero-length array is not portable C, so we keep a single-slot sentinel
   placeholder that is never iterated (kUniversalPatchesCount = 0u).  The
   sentinel's Apply pointer is NULL — exercising it would trap, which is
   the intended outcome if the count is ever incorrectly raised. */
CONST PATCH_DESC kUniversalPatches[] = {
  { .Name = "(retired)", .Scope = SCOPE_UNIVERSAL, .Mandatory = FALSE, .Apply = NULL },
};
CONST UINTN kUniversalPatchesCount = 0u;
