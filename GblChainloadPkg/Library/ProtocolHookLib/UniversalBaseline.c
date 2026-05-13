/** @file UniversalBaseline.c — universal-mode hook policy implementation.

    These policies are unconditional — they run for mode-1, mode-2, and
    mode-3 (and observation-only debug builds).  The contract they enforce
    is "no persistent state mutation during chainload": WRITE_CONFIG and
    VBDeviceResetState are swallowed so ABL cannot persist lock-state
    experiments back to RPMB; TZ_BLOW_SW_FUSE_ID is dropped so the
    soft-fuse is not advanced; OplusSec 0x0A write_rpmb_boot_info is
    dropped so the RPMB boot-info slot is not overwritten.

    Mode-specific overlays (Mode1Overlay.c, etc.) layer on top; they are
    called from slot wrappers before or after these universal checks as
    appropriate.
**/

#include "UniversalBaseline.h"
#include <Library/DebugLib.h>
#include <Library/LogFsLib.h>  /* GBL_DBG_LOGFS_ONLY level bit */

/* Match the SCM SIP that ScmHook.c decodes (existing constant). */
#define SCM_SIP_TZ_BLOW_SW_FUSE_ID  0x02000801U

/* Match the OplusSec cmd id that QseecomHook.c decodes. */
#define OPLUSSEC_CMD_WRITE_RPMB_BOOT_INFO  0x0AU

EFI_STATUS EFIAPI
UniversalPolicy_OnVbWriteConfig (
  IN UINT32  Op,
  IN VOID   *Buf,
  IN UINT32  BufLen
  )
{
  /* Op should be WRITE_CONFIG; caller has already classified.
     Just emit a diagnostic log and return SUCCESS. */
  DEBUG ((GBL_DBG_LOGFS_ONLY,
          "vb-rwstate | op=WRITE_CONFIG | bufLen=%u | swallowed (universal)\n",
          BufLen));
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
UniversalPolicy_OnVbReset (VOID)
{
  DEBUG ((GBL_DBG_LOGFS_ONLY, "vb-reset | swallowed (universal)\n"));
  return EFI_SUCCESS;
}

BOOLEAN
UniversalPolicy_ShouldDropScmSip (
  IN  UINT32       SmcId,
  OUT EFI_STATUS  *FakeStatus
  )
{
  if (SmcId == SCM_SIP_TZ_BLOW_SW_FUSE_ID) {
    *FakeStatus = EFI_SUCCESS;
    DEBUG ((GBL_DBG_LOGFS_ONLY,
            "scm-sip | smcid=0x%08x(TZ_BLOW_SW_FUSE_ID) | DROPPED (universal)\n",
            SmcId));
    return TRUE;
  }
  return FALSE;
}

BOOLEAN
UniversalPolicy_ShouldDropQseeOplusSec (
  IN  UINT32       CmdId,
  OUT EFI_STATUS  *FakeStatus
  )
{
  if (CmdId == OPLUSSEC_CMD_WRITE_RPMB_BOOT_INFO) {
    *FakeStatus = EFI_SUCCESS;
    DEBUG ((GBL_DBG_LOGFS_ONLY,
            "qsee-oplussec | cmd=0x%02x(write_rpmb_boot_info) | DROPPED (universal)\n",
            CmdId));
    return TRUE;
  }
  return FALSE;
}
