/** @file PatchEngine.c — patch table iterator.

    Walks gPatchTable, calls each entry's Apply function pointer, and
    accumulates results in a PATCH_RESULT.  The table itself is supplied
    externally: by PatchTable.c in EDK-II builds (Task 19) or by the
    test file via extern override in host builds.
**/

#include "../../../Include/Library/DynamicPatchLib.h"

#ifndef __HOST_BUILD__
#include <Library/DebugLib.h>
#include <Library/UefiLib.h>
#endif

/* Patch table provided externally by the aggregator (Task 19).
   Host tests override these symbols via extern declarations. */
CONST PATCH_DESC  *gPatchTable    = NULL;
UINTN              gPatchTableLen = 0;

VOID
DynamicPatch_Apply (
  IN OUT UINT8         *Buf,
  IN     UINT32         Size,
  OUT    PATCH_RESULT  *Result
  )
{
  UINTN i;

  if (Result == NULL) {
    return;
  }

  Result->AppliedCount = 0;
  Result->MissedCount  = 0;
  Result->WorstOutcome = PATCH_RESULT_OK;

  if (gPatchTable == NULL || gPatchTableLen == 0) {
    return;  /* No patches configured — vacuously OK. */
  }

  for (i = 0; i < gPatchTableLen; ++i) {
    CONST PATCH_DESC *P = &gPatchTable[i];
    PATCH_OUTCOME     O = P->Apply (Buf, Size);

#ifndef __HOST_BUILD__
    {
      CONST CHAR8  *OutcomeName =
        (O == PATCH_OK)        ? "OK"        :
        (O == PATCH_MISS)      ? "MISS"      :
        (O == PATCH_AMBIGUOUS) ? "AMBIGUOUS" : "?";
      CONST CHAR8  *ScopeName =
        (P->Scope == SCOPE_UNIVERSAL)   ? "universal"   :
        (P->Scope == SCOPE_OEM_ONEPLUS) ? "oem-oneplus" :
        (P->Scope == SCOPE_MODE_1)      ? "mode-1"      : "unknown";
      DEBUG ((DEBUG_INFO,
              "DynamicPatch: %a [%a, %a] -> %a\n",
              P->Name, ScopeName,
              P->Mandatory ? "mandatory" : "optional",
              OutcomeName));
    }
#endif

    if (O == PATCH_OK) {
      ++Result->AppliedCount;
    } else {
      /* PATCH_MISS and PATCH_AMBIGUOUS both count as a miss.
         PATCH_AMBIGUOUS means the engine cannot safely choose a match,
         so the patch is skipped — same accounting as a clean miss. */
      ++Result->MissedCount;
#ifndef __HOST_BUILD__
      /* Emit a screen-visible line for non-OK outcomes so missed patches
         are visible during device boot without requiring GBL_DEBUG=1. */
      Print (L"DynamicPatch: %a (%a) -> %a\n",
             P->Name,
             P->Mandatory ? "mandatory" : "optional",
             (O == PATCH_AMBIGUOUS) ? "AMBIGUOUS" : "MISS");
#endif
      if (P->Mandatory) {
        if (Result->WorstOutcome < PATCH_RESULT_MANDATORY_MISS) {
          Result->WorstOutcome = PATCH_RESULT_MANDATORY_MISS;
        }
      } else {
        if (Result->WorstOutcome < PATCH_RESULT_OPTIONAL_MISS) {
          Result->WorstOutcome = PATCH_RESULT_OPTIONAL_MISS;
        }
      }
    }
  }
}
