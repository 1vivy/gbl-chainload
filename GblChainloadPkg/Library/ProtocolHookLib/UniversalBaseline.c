/** @file UniversalBaseline.c — universal-mode hook policy implementation.

    These policies are unconditional.  The universal contract covers three
    SCM SIPs that must never reach TZ firmware:

      TZ_BLOW_SW_FUSE_ID (0x02000801)
        — soft-fuse advancement suppressed unconditionally.

      TZ_UPDATE_ROLLBACK_VERSION_ID
        — RPMB-stored rollback floor; dropping pins the minimum OS version
          and prevents the factory-reset funnel when reflashing older images.

      TZ_UPDATE_ROLLBACK_VERSION_IF_AB_PARTITION_FEATURE_ENABLED_ID
        — A/B-aware rollback bump; same rationale as above.

    KM 0x207 SET_VERSION is intentionally NOT dropped here — that travels
    over QSEECOM (not SCM SIP) and is handled by mode-specific overlays.

    Mode-specific overlays (Mode1Overlay.c, etc.) layer on top; they are
    called from slot wrappers before or after these universal checks as
    appropriate.
**/

#include "UniversalBaseline.h"
#include <Library/DebugLib.h>
#include <Library/GblLog.h>

/* Match the SCM SIPs that ScmHook.c decodes (constants must agree). */
#define SCM_SIP_TZ_BLOW_SW_FUSE_ID          0x02000801U
#define SCM_SIP_TZ_UPDATE_ROLLBACK_VER      0x0200011EU
#define SCM_SIP_TZ_UPDATE_ROLLBACK_VER_AB   0x32000110U

BOOLEAN
UniversalPolicy_ShouldDropScmSip (
  IN  UINT32       SmcId,
  OUT EFI_STATUS  *FakeStatus
  )
{
  switch (SmcId) {

    case SCM_SIP_TZ_BLOW_SW_FUSE_ID:
      *FakeStatus = EFI_SUCCESS;
      GBL_INFO ("scm-sip | smcid=0x%08x(TZ_BLOW_SW_FUSE_ID) | DROPPED (universal)\n",
                SmcId);
      return TRUE;

    case SCM_SIP_TZ_UPDATE_ROLLBACK_VER:
      *FakeStatus = EFI_SUCCESS;
      GBL_INFO ("scm-sip | smcid=0x%08x(TZ_UPDATE_ROLLBACK_VERSION_ID)"
                " | DROPPED (universal)\n",
                SmcId);
      return TRUE;

    case SCM_SIP_TZ_UPDATE_ROLLBACK_VER_AB:
      *FakeStatus = EFI_SUCCESS;
      GBL_INFO ("scm-sip | smcid=0x%08x"
                "(TZ_UPDATE_ROLLBACK_VERSION_IF_AB_PARTITION_FEATURE_ENABLED_ID)"
                " | DROPPED (universal)\n",
                SmcId);
      return TRUE;

    default:
      return FALSE;
  }
}
