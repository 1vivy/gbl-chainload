/** @file FakelockOverlay.c — fakelock / persistence-suppression hook policy
  implementation.

  Contains fakelock and persistence-suppression policies; activation is
  runtime-gated by callers on gManifest.WantFakelockHook, so these symbols
  compile in every build and are dead-stripped if no call site references
  them.

  THE BRICK-SAFETY INVARIANT
  --------------------------
  The device's TRUE state is *unlocked* (that is how we are running a
  chain-loaded EFI at all). Fakelock makes ABL *appear* locked at runtime so
  attestation reports locked — but the persistent lock-state store (RPMB) must
  NEVER drift to "locked", or a future boot without our chain-load present finds
  a locked record over unofficial images and refuses to boot (brick).

  So every lock-state→persistence path is held to one rule:

      persistence may only ever move TOWARD unlocked, never toward locked.

  Enforced per path by mechanism (split because we can only rewrite payloads
  whose layout we know):

    VB READ_CONFIG / VBDeviceInit  — read-side spoof: force the downstream view
        is_unlocked / is_unlock_critical -> 0 (locked) so ABL behaves locked.
        (FakelockOverlay_OnVbReadConfig_Post / _OnVbDeviceInit_PrePost.)

    VB WRITE_CONFIG  — write-side HEAL: the payload is an inline DeviceInfo whose
        layout we own, so rewrite both flags -> unlocked and FORWARD. The last
        canonical write to land heals RPMB toward the true state.
        (FakelockOverlay_OnVbWriteConfig.)

    VB Reset  — SWALLOW: no payload to heal; a reset could only re-assert a
        locked/default record, which the invariant forbids forwarding.
        (FakelockOverlay_OnVbReset.)

    OplusSec cmd 0x0A (write_rpmb_boot_info)  — DROP: opaque OEM blob, no decoded
        layout, so it cannot be healed; refuse it. OplusSec is an OS-facing TZ
        app, so a heal path is not pursued.
        (FakelockOverlay_ShouldDropQseeOplusSec.)

    KM cmd 0x203 (WRITE_KM_DEVICE_STATE)  — DROP: the send buffer carries only a
        pointer ({cmd, addr_lo, addr_hi}) to an opaque KM device-state struct we
        have no ground truth for (infiniti never issues 0x203), so it cannot be
        safely healed today; refuse it. RE follow-up to recover the pointee
        layout so it can be healed instead of dropped — see
        docs/project/re-findings.md "Device: macan / sm8845".
        (FakelockOverlay_ShouldDropKmDeviceStateWrite.)
**/
#include "FakelockOverlay.h"

#include <Library/DebugLib.h>
#include <Library/GblLog.h>
#include <Library/DeviceInfo.h>

#define OPLUSSEC_CMD_WRITE_RPMB_BOOT_INFO  0x0AU
#define KM_CMD_WRITE_KM_DEVICE_STATE       0x00000203U

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

BOOLEAN EFIAPI
FakelockOverlay_OnVbWriteConfig (
  IN     UINT32  Op,
  IN OUT VOID   *Buf,
  IN     UINT32  BufLen
  )
{
  UINT8   *B;
  UINTN    IsUnlockedOff;
  UINTN    IsUnlockCriticalOff;
  BOOLEAN  OldUnlocked;
  BOOLEAN  OldUnlockCritical;

  /* Self-enforcing contract: this heal only has meaning for WRITE_CONFIG.
     Refuse (fail-safe swallow) if a future caller ever routes another op
     through here, rather than silently healing+forwarding a non-write. */
  if (Op != WRITE_CONFIG) {
    GBL_INFO ("vb-fakelock | OnVbWriteConfig called with op=%u (!=WRITE_CONFIG) "
              "— swallow (contract guard)\n", Op);
    return FALSE;
  }

  if (Buf == NULL) {
    /* Nothing to heal — fail safe: tell the caller to swallow rather than
       forward a record we cannot prove is unlocked. */
    GBL_INFO ("vb-fakelock | WRITE_CONFIG | NULL buf — swallow (cannot heal)\n");
    return FALSE;
  }

  B                   = (UINT8 *)Buf;
  IsUnlockedOff       = Mode1OffsetOfIsUnlocked ();
  IsUnlockCriticalOff = Mode1OffsetOfIsUnlockCritical ();

  if ((UINTN)BufLen <= IsUnlockedOff ||
      (UINTN)BufLen <= IsUnlockCriticalOff) {
    /* Can't locate the lock flags in this buffer — fail safe by swallowing.
       Forwarding an un-healed buffer risks persisting a fake-locked record. */
    GBL_INFO ("vb-fakelock | WRITE_CONFIG | buffer too small len=%u need>%u — "
              "swallow (cannot heal)\n",
              BufLen, (UINT32)IsUnlockCriticalOff);
    return FALSE;
  }

  /* Heal toward the TRUE (unlocked) state: 1 == unlocked (read-side spoof
     clears these to 0). Forwarding the healed buffer means the last
     device-state write to land in RPMB leaves the device recoverable. */
  OldUnlocked       = B[IsUnlockedOff]       ? TRUE : FALSE;
  OldUnlockCritical = B[IsUnlockCriticalOff] ? TRUE : FALSE;
  B[IsUnlockedOff]       = 1;
  B[IsUnlockCriticalOff] = 1;

  GBL_INFO ("vb-fakelock | WRITE_CONFIG | bufLen=%u | healed->unlocked "
            "(is_unlocked %u->1 | is_unlock_critical %u->1) | forwarding\n",
            BufLen, (UINT32)OldUnlocked, (UINT32)OldUnlockCritical);
  return TRUE;
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

BOOLEAN
FakelockOverlay_ShouldDropKmDeviceStateWrite (
  IN  UINT32       CmdId,
  OUT EFI_STATUS  *FakeStatus
  )
{
  if (CmdId == KM_CMD_WRITE_KM_DEVICE_STATE) {
    *FakeStatus = EFI_SUCCESS;
    GBL_INFO ("qsee-km | cmd=0x%08x(WRITE_KM_DEVICE_STATE) | DROPPED "
              "(mode-1: refuse RPMB lock-state persist)\n",
              CmdId);
    return TRUE;
  }
  return FALSE;
}
