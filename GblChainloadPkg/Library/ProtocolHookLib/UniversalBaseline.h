/** @file UniversalBaseline.h — universal hook policy declarations.
    These always run regardless of manifest caps; per-cap overlays
    (FakelockOverlay, ProfileOverlay) layer on top under runtime gates.
**/
#ifndef UNIVERSAL_BASELINE_H_
#define UNIVERSAL_BASELINE_H_

#include <Uefi.h>
#include "HookCommon.h"

/* SCM policy: TZ_BLOW_SW_FUSE_ID and anti-rollback SmcId drops. */

/** If SmcId is one of the universally-dropped SIPs (soft-fuse-blow or
    either TZ anti-rollback version-set call), returns TRUE and writes
    EFI_SUCCESS into FakeStatus; caller short-circuits without forwarding
    the SMC.  Returns FALSE for any other SmcId — caller proceeds normally. **/
BOOLEAN
UniversalPolicy_ShouldDropScmSip (
  IN  UINT32       SmcId,
  OUT EFI_STATUS  *FakeStatus
  );

#endif
