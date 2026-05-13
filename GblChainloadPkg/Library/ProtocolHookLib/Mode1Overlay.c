/** @file Mode1Overlay.c — mode-1-scope hook policy implementation.

  Contains the two fakelock mutators that are exclusive to mode-1:

    Mode1Policy_OnVbReadConfig_Post  — post-call: clears is_unlocked +
        is_unlock_critical in the raw READ_CONFIG device-state buffer
        (which is a DeviceInfo blob). Uses offset arithmetic identical
        to the dirty VbForceDeviceInfoBufferLocked helper it replaces.

    Mode1Policy_OnVbDeviceInit_PrePost — pre/post-call: clears the same
        two fields in the device_info_vb_t struct passed to VBDeviceInit.

  Both functions are compiled-out entirely in non-mode-1 builds via the
  GBL_MODE == 1 guard in Mode1Overlay.h.
**/
#include "Mode1Overlay.h"

#if (GBL_MODE == 1)

#include <Library/DebugLib.h>
#include <Library/DeviceInfo.h>
#include <Library/LogFsLib.h>  /* GBL_DBG_LOGFS_ONLY level bit */

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
Mode1Policy_OnVbReadConfig_Post (
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

  DEBUG ((GBL_DBG_LOGFS_ONLY,
          "vb-fakelock | READ_CONFIG | is_unlocked %u->0 | is_unlock_critical %u->0\n",
          (UINT32)OldUnlocked, (UINT32)OldUnlockCritical));

  return OrigStatus;
}

VOID EFIAPI
Mode1Policy_OnVbDeviceInit_PrePost (
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

  DEBUG ((GBL_DBG_LOGFS_ONLY,
          "vb-fakelock | VBDeviceInit/%a | is_unlocked %u->0 | is_unlock_critical %u->0\n",
          IsPre ? "pre" : "post",
          (UINT32)OldUnlocked, (UINT32)OldUnlockCritical));
}

#endif /* GBL_MODE == 1 */
