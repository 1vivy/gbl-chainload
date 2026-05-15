/** @file PatchEngine.c — patch table iterator.

    Walks gPatchTable, calls each entry's Apply function pointer, and
    accumulates results in a PATCH_RESULT.  The table itself is supplied
    externally: by PatchTable.c in EDK-II builds (Task 19) or by the
    test file via extern override in host builds.
**/

#include "../../../Include/Library/DynamicPatchLib.h"
#include "ScanLib.h"
#include "../../../../tools/shared/patch_signatures.h"

#ifndef __HOST_BUILD__
#include <Library/DebugLib.h>
#include <Library/UefiLib.h>
#else
#include <stdio.h>
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

    {
      CONST CHAR8  *OutcomeName =
        (O == PATCH_OK)        ? "OK"        :
        (O == PATCH_MISS)      ? "MISS"      :
        (O == PATCH_AMBIGUOUS) ? "AMBIGUOUS" : "?";
      CONST CHAR8  *ScopeName =
        (P->Scope == SCOPE_UNIVERSAL)   ? "universal"   :
        (P->Scope == SCOPE_OEM_ONEPLUS) ? "oem-oneplus" :
        (P->Scope == SCOPE_MODE_1)      ? "mode-1"      : "unknown";
#ifdef __HOST_BUILD__
      fprintf (stderr,
               "DynamicPatch: %s [%s, %s] -> %s\n",
               P->Name, ScopeName,
               P->Mandatory ? "mandatory" : "optional",
               OutcomeName);
#else
      DEBUG ((DEBUG_INFO,
              "DynamicPatch: %a [%a, %a] -> %a\n",
              P->Name, ScopeName,
              P->Mandatory ? "mandatory" : "optional",
              OutcomeName));
#endif
    }

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

  /* Post-patch efisp invariant: the patched PE must NOT contain any
     UTF-16 LE "efisp" bytes (kEfispUtf16Pattern, 10 bytes).  Refines
     c49f1a8 from blanket allow-on-failure to an absence-of-efisp gate
     that catches signature-table drift early.

     Uses ScanFor (not gbl_contains_utf16_efisp from efisp_scan.h) because
     ScanFor is EDK2-native and kEfispUtf16Pattern is the exact 10-byte
     sequence that patch1 zeroes — so a SCAN_NOT_FOUND here confirms
     patch1 did its job.  SCAN_FOUND means the signature table does not
     cover this ABL variant; we treat that as a mandatory miss so the
     caller (BootFlow.c Tier 2) fails cleanly and Tier 3 (Fastboot) takes
     over. */
  if (Buf != NULL && Size != 0) {
    UINT32 EfispOff = 0;
    SCAN_RESULT EfispScan = ScanFor (
                              Buf,
                              Size,
                              kEfispUtf16Pattern,
                              NULL,
                              sizeof (kEfispUtf16Pattern),
                              &EfispOff
                              );
    if (EfispScan == SCAN_FOUND || EfispScan == SCAN_AMBIGUOUS) {
#ifdef __HOST_BUILD__
      fprintf (stderr,
               "DynamicPatch: efisp bytes still present after patches "
               "(offset 0x%x); refusing — signature table likely missing "
               "this ABL variant\n",
               (unsigned)EfispOff);
#else
      DEBUG ((DEBUG_ERROR,
              "DynamicPatch: efisp bytes still present after patches "
              "(offset 0x%x); refusing — signature table likely missing "
              "this ABL variant\n",
              (UINT32)EfispOff));
      Print (L"DynamicPatch: FATAL — efisp invariant violated; "
             L"signature table missing this ABL variant\n");
#endif
      Result->WorstOutcome = PATCH_RESULT_MANDATORY_MISS;
    }
  }
}
