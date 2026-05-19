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
#ifdef __HOST_BUILD__
#include "PatchScope.h"
#endif

extern CONST PATCH_DESC kUniversalPatches[];
extern CONST UINTN      kUniversalPatchesCount;
extern CONST PATCH_DESC kOemOneplusPatches[];
extern CONST UINTN      kOemOneplusPatchesCount;

/* kMode1Patches is needed when GBL_MODE >= 1 (on-device) *or* when building
   for the host (abl-patcher): the object is always linked in both cases and
   EnsureInitScoped decides at runtime whether to include the patches. */
#if (GBL_MODE >= 1) || defined(__HOST_BUILD__)
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

#ifdef __HOST_BUILD__
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
#endif /* __HOST_BUILD__ */
