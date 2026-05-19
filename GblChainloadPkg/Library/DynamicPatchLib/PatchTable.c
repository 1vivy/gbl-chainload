/** @file PatchTable.c — assembles the runtime patch table for the active GBL_MODE.

    Order: universal first, then OEM, then mode-specific.
    Called explicitly by EDK-II callers (BootFlow.c) via DynamicPatchLib_EnsureInit()
    before DynamicPatch_Apply().  Host tests bypass this entirely by assigning
    gPatchTable directly.

    Host callers (abl-patcher) use DynamicPatchLib_EnsureInitScoped() for
    runtime patch-scope selection; see PatchScope.h.
**/

#include "../../../Include/Library/PatchDesc.h"
#include "../../../Include/Library/DynamicPatchLib.h"
#include "PatchScope.h"

extern CONST PATCH_DESC kUniversalPatches[];
extern CONST UINTN      kUniversalPatchesCount;
extern CONST PATCH_DESC kOemOneplusPatches[];
extern CONST UINTN      kOemOneplusPatchesCount;

#if (GBL_MODE >= 1)
extern CONST PATCH_DESC kMode1Patches[];
extern CONST UINTN      kMode1PatchesCount;
#endif

/* Unconditional externs for the runtime host aggregator: kMode1Patches always
   exists in the linked object (mode_1/mode_1.c is in the abl-patcher Makefile);
   only its *inclusion* in the table is conditional. */
#ifdef __HOST_BUILD__
extern CONST PATCH_DESC kMode1Patches[];
extern CONST UINTN      kMode1PatchesCount;
#endif

#define MAX_PATCHES  16

STATIC PATCH_DESC  gAggregated[MAX_PATCHES];
STATIC UINTN       gAggregatedLen = 0;
STATIC BOOLEAN     gAggregateInit = FALSE;

/* Defined in PatchEngine.c; aggregator populates them. */
extern CONST PATCH_DESC  *gPatchTable;
extern UINTN              gPatchTableLen;

STATIC VOID
InitAggregate (VOID)
{
  UINTN n = 0;
  UINTN i;

  for (i = 0; i < kUniversalPatchesCount && n < MAX_PATCHES; ++i) {
    gAggregated[n++] = kUniversalPatches[i];
  }
  for (i = 0; i < kOemOneplusPatchesCount && n < MAX_PATCHES; ++i) {
    gAggregated[n++] = kOemOneplusPatches[i];
  }
#if (GBL_MODE >= 1)
  for (i = 0; i < kMode1PatchesCount && n < MAX_PATCHES; ++i) {
    gAggregated[n++] = kMode1Patches[i];
  }
#endif
  gAggregatedLen = n;
  gPatchTable    = gAggregated;
  gPatchTableLen = n;
  gAggregateInit = TRUE;
}

VOID
DynamicPatchLib_EnsureInit (VOID)
{
  if (!gAggregateInit) {
    InitAggregate ();
  }
}

/* Runtime scope aggregator for host callers (abl-patcher).
   Builds the table from: universal, then (if oem != NONE) the OEM group,
   then (if include_mode1) the mode_1 group.  Replaces the compile-time
   GBL_MODE aggregation for tools that serve multiple modes from one binary. */
void
DynamicPatchLib_EnsureInitScoped (GBL_OEM oem, int include_mode1)
{
  UINTN n = 0;
  UINTN i;

  for (i = 0; i < kUniversalPatchesCount && n < MAX_PATCHES; ++i)
    gAggregated[n++] = kUniversalPatches[i];
  if (oem == GBL_OEM_ONEPLUS)
    for (i = 0; i < kOemOneplusPatchesCount && n < MAX_PATCHES; ++i)
      gAggregated[n++] = kOemOneplusPatches[i];
  if (include_mode1)
    for (i = 0; i < kMode1PatchesCount && n < MAX_PATCHES; ++i)
      gAggregated[n++] = kMode1Patches[i];
  gAggregatedLen = n;
  gPatchTable    = gAggregated;
  gPatchTableLen = n;
  gAggregateInit = TRUE;
}
