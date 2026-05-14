/** @file UniversalBaseline.c — universal-mode hook policy implementation.

    These policies are unconditional.  The universal contract is narrow:
    drop TZ_BLOW_SW_FUSE_ID so the soft-fuse is not advanced.  Other
    persistence policies live in mode-specific overlays or dedicated hooks.

    Mode-specific overlays (Mode1Overlay.c, etc.) layer on top; they are
    called from slot wrappers before or after these universal checks as
    appropriate.
**/

#include "UniversalBaseline.h"
#include <Library/DebugLib.h>
#include <Library/GblLog.h>

/* Match the SCM SIP that ScmHook.c decodes (existing constant). */
#define SCM_SIP_TZ_BLOW_SW_FUSE_ID  0x02000801U

BOOLEAN
UniversalPolicy_ShouldDropScmSip (
  IN  UINT32       SmcId,
  OUT EFI_STATUS  *FakeStatus
  )
{
  if (SmcId == SCM_SIP_TZ_BLOW_SW_FUSE_ID) {
    *FakeStatus = EFI_SUCCESS;
    GBL_INFO ("scm-sip | smcid=0x%08x(TZ_BLOW_SW_FUSE_ID) | DROPPED (universal)\n",
              SmcId);
    return TRUE;
  }
  return FALSE;
}
