/** @file UniversalBaseline.h — universal-mode hook policy declarations.
    These run on every GBL_MODE; the per-mode overlay layers on top.
**/
#ifndef UNIVERSAL_BASELINE_H_
#define UNIVERSAL_BASELINE_H_

#include <Uefi.h>
#include "HookCommon.h"

/* SCM policy: TZ_BLOW_SW_FUSE_ID drop. */

/** If SmcId is the soft-fuse-blow SIP, returns TRUE and writes EFI_SUCCESS
    into FakeStatus; caller short-circuits without forwarding the SMC.
    Returns FALSE for any other SmcId — caller proceeds normally. **/
BOOLEAN
UniversalPolicy_ShouldDropScmSip (
  IN  UINT32       SmcId,
  OUT EFI_STATUS  *FakeStatus
  );

#endif
