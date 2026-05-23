/** @file PatchTable.c — assembles the runtime patch table.

    Firmware build: universal (retired) + abl_permissive groups, unconditionally.
    Host build:     additionally links the OEM group(s) and exposes the runtime
                    scope selector DynamicPatchLib_EnsureInitScoped() so tools
                    (abl-patcher) can pick OEM and ABL-permissive inclusion at
                    invocation time.  See PatchScope.h.

    Order in either path: universal first, then OEM (host only), then
    abl_permissive.  Called from BootFlow.c via DynamicPatchLib_EnsureInit()
    before DynamicPatch_Apply().  Host tests bypass this entirely by assigning
    gPatchTable directly.
**/

#include "../../../Include/Library/PatchDesc.h"
#include "../../../Include/Library/DynamicPatchLib.h"
#ifdef __HOST_BUILD__
#include "PatchScope.h"
#endif

extern CONST PATCH_DESC kUniversalPatches[];           /* retired/block_efisp_recursion.c; drops in Task 10 */
extern CONST UINTN      kUniversalPatchesCount;
extern CONST PATCH_DESC kAblPermissiveLibavbPatches[];   /* was part of kMode1Patches */
extern CONST UINTN      kAblPermissiveLibavbPatchesCount;
extern CONST PATCH_DESC kAblPermissiveFastbootGatePatches[];
extern CONST UINTN      kAblPermissiveFastbootGatePatchesCount;
#ifdef __HOST_BUILD__
extern CONST PATCH_DESC kOemOplusPatches[];              /* was kOemOneplusPatches */
extern CONST UINTN      kOemOplusPatchesCount;
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
#ifdef __HOST_BUILD__
  /* OEM source is only compiled into host tools; firmware build does not
     link oem/oplus/bypass_warning.c. */
  for (i = 0; i < kOemOplusPatchesCount && n < MAX_PATCHES; ++i) {
    gAggregated[n++] = kOemOplusPatches[i];
  }
#endif
  for (i = 0; i < kAblPermissiveLibavbPatchesCount && n < MAX_PATCHES; ++i) {
    gAggregated[n++] = kAblPermissiveLibavbPatches[i];
  }
  for (i = 0; i < kAblPermissiveFastbootGatePatchesCount && n < MAX_PATCHES; ++i) {
    gAggregated[n++] = kAblPermissiveFastbootGatePatches[i];
  }
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
   Builds the table from: universal (retired patch1), then (if oem != NONE)
   the OEM group, then (if include_abl_permissive) the ABL-permissive groups.
   Lets one host binary serve any (oem, abl_permissive) combination. */
void
DynamicPatchLib_EnsureInitScoped (GBL_OEM oem, int include_abl_permissive)
{
  UINTN n = 0;
  UINTN i;

  for (i = 0; i < kUniversalPatchesCount && n < MAX_PATCHES; ++i)
    gAggregated[n++] = kUniversalPatches[i];
  if (oem == GBL_OEM_OPLUS)
    for (i = 0; i < kOemOplusPatchesCount && n < MAX_PATCHES; ++i)
      gAggregated[n++] = kOemOplusPatches[i];
  if (include_abl_permissive) {
    for (i = 0; i < kAblPermissiveLibavbPatchesCount && n < MAX_PATCHES; ++i)
      gAggregated[n++] = kAblPermissiveLibavbPatches[i];
    for (i = 0; i < kAblPermissiveFastbootGatePatchesCount && n < MAX_PATCHES; ++i)
      gAggregated[n++] = kAblPermissiveFastbootGatePatches[i];
  }
  gAggregatedLen = n;
  gPatchTable    = gAggregated;
  gPatchTableLen = n;
  gAggregateInit = TRUE;
}
#endif /* __HOST_BUILD__ */
