/** @file FakelockOverlay.c — fakelock / persistence-suppression hook policy
  implementation.

  Contains fakelock and persistence-suppression policies; activation is
  runtime-gated by callers on gManifest.WantFakelockHook, so these symbols
  compile in every build and are dead-stripped if no call site references
  them.

    FakelockOverlay_OnVbReadConfig_Post  — post-call: clears is_unlocked +
        is_unlock_critical in the raw READ_CONFIG device-state buffer
        (which is a DeviceInfo blob). Uses offset arithmetic identical
        to the dirty VbForceDeviceInfoBufferLocked helper it replaces.

    FakelockOverlay_OnVbDeviceInit_PrePost — pre/post-call: clears the same
        two fields in the device_info_vb_t struct passed to VBDeviceInit.
**/
#include "FakelockOverlay.h"

#include <Library/DebugLib.h>
#include <Library/GblLog.h>
#include <Library/DeviceInfo.h>

#define OPLUSSEC_CMD_WRITE_RPMB_BOOT_INFO  0x0AU

/* --------------------------------------------------------------------------
 * Internal helpers (mirrors dirty VbOffsetOf* / VbForceDeviceInfoBufferLocked)
 * -------------------------------------------------------------------------- */

STATIC UINTN
Mode1OffsetOfIsUnlocked (VOID)
{
  return (UINTN)&(((DeviceInfo *)0)->is_unlocked);
}

STATIC UINTN
Mode1OffsetOfIsUnlockCritical (VOID)
{
  return (UINTN)&(((DeviceInfo *)0)->is_unlock_critical);
}

/* --------------------------------------------------------------------------
 * Public policy functions
 * -------------------------------------------------------------------------- */

EFI_STATUS EFIAPI
FakelockOverlay_OnVbReadConfig_Post (
  IN  EFI_STATUS  OrigStatus,
  IN  VOID       *Buf,
  IN  UINT32      BufLen
  )
{
  UINT8   *B;
  UINTN    IsUnlockedOff;
  UINTN    IsUnlockCriticalOff;
  BOOLEAN  OldUnlocked;
  BOOLEAN  OldUnlockCritical;

  if (EFI_ERROR (OrigStatus) || Buf == NULL) {
    return OrigStatus;
  }

  B                   = (UINT8 *)Buf;
  IsUnlockedOff       = Mode1OffsetOfIsUnlocked ();
  IsUnlockCriticalOff = Mode1OffsetOfIsUnlockCritical ();

  if ((UINTN)BufLen <= IsUnlockedOff ||
      (UINTN)BufLen <= IsUnlockCriticalOff) {
    DEBUG ((DEBUG_WARN,
            "vb-fakelock | READ_CONFIG | buffer too small len=%u need>%u\n",
            BufLen, (UINT32)IsUnlockCriticalOff));
    return OrigStatus;
  }

  OldUnlocked       = B[IsUnlockedOff]       ? TRUE : FALSE;
  OldUnlockCritical = B[IsUnlockCriticalOff] ? TRUE : FALSE;
  B[IsUnlockedOff]       = 0;
  B[IsUnlockCriticalOff] = 0;

  GBL_INFO ("vb-fakelock | READ_CONFIG | is_unlocked %u->0 | is_unlock_critical %u->0\n",
            (UINT32)OldUnlocked, (UINT32)OldUnlockCritical);

  return OrigStatus;
}

VOID EFIAPI
FakelockOverlay_OnVbDeviceInit_PrePost (
  IN OUT device_info_vb_t *Devinfo,
  IN     BOOLEAN           IsPre
  )
{
  BOOLEAN OldUnlocked;
  BOOLEAN OldUnlockCritical;

  if (Devinfo == NULL) {
    return;
  }

  OldUnlocked       = Devinfo->is_unlocked       ? TRUE : FALSE;
  OldUnlockCritical = Devinfo->is_unlock_critical ? TRUE : FALSE;
  Devinfo->is_unlocked        = FALSE;
  Devinfo->is_unlock_critical = FALSE;

  GBL_INFO ("vb-fakelock | VBDeviceInit/%a | is_unlocked %u->0 | is_unlock_critical %u->0\n",
            IsPre ? "pre" : "post",
            (UINT32)OldUnlocked, (UINT32)OldUnlockCritical);
}

EFI_STATUS EFIAPI
FakelockOverlay_OnVbWriteConfig (
  IN UINT32  Op,
  IN VOID   *Buf,
  IN UINT32  BufLen
  )
{
  GBL_INFO ("vb-rwstate | op=WRITE_CONFIG | bufLen=%u | swallowed (mode-1)\n",
            BufLen);
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
FakelockOverlay_OnVbReset (VOID)
{
  GBL_INFO ("vb-reset | swallowed (mode-1)\n");
  return EFI_SUCCESS;
}

BOOLEAN
FakelockOverlay_ShouldDropQseeOplusSec (
  IN  UINT32       CmdId,
  OUT EFI_STATUS  *FakeStatus
  )
{
  if (CmdId == OPLUSSEC_CMD_WRITE_RPMB_BOOT_INFO) {
    *FakeStatus = EFI_SUCCESS;
    GBL_INFO ("qsee-oplussec | cmd=0x%02x(write_rpmb_boot_info) | DROPPED (mode-1)\n",
              CmdId);
    return TRUE;
  }
  return FALSE;
}
