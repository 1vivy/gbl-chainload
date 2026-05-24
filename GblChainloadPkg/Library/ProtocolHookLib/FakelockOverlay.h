/** @file FakelockOverlay.h — fakelock / persistence-suppression hook policy
    declarations. Activation is runtime-gated on gManifest.WantFakelockHook
    at the call sites; declarations are unconditional so the wrappers can
    take the gate inline. **/
#ifndef FAKELOCK_OVERLAY_H_
#define FAKELOCK_OVERLAY_H_

#include <Uefi.h>
#include <Protocol/EFIVerifiedBoot.h>
#include "HookCommon.h"

/** Fakelock policy for VBRwDeviceState(READ_CONFIG) post-call mutator.
    Clears is_unlocked and is_unlock_critical in the returned device_info_vb_t.
    Returns OrigStatus unchanged (pass-through). **/
EFI_STATUS EFIAPI
FakelockOverlay_OnVbReadConfig_Post (
  IN  EFI_STATUS  OrigStatus,
  IN  VOID       *Buf,
  IN  UINT32      BufLen
  );

/** Fakelock policy for VBDeviceInit pre/post clear.
    Pass IsPre=TRUE before calling original; IsPre=FALSE after.
    Safe to call with Devinfo==NULL (no-op). **/
VOID EFIAPI
FakelockOverlay_OnVbDeviceInit_PrePost (
  IN OUT device_info_vb_t *Devinfo,
  IN     BOOLEAN           IsPre
  );

/** Fakelock policy for VBRwDeviceState(WRITE_CONFIG). Returns EFI_SUCCESS and
    does NOT forward to the original. **/
EFI_STATUS EFIAPI
FakelockOverlay_OnVbWriteConfig (
  IN UINT32  Op,
  IN VOID   *Buf,
  IN UINT32  BufLen
  );

/** Fakelock policy for VBDeviceResetState. Returns EFI_SUCCESS without
    forwarding. **/
EFI_STATUS EFIAPI
FakelockOverlay_OnVbReset (VOID);

/** Fakelock policy for OplusSec cmd 0x0A write_rpmb_boot_info. Caller already
    determined Handle == OplusSec. **/
BOOLEAN
FakelockOverlay_ShouldDropQseeOplusSec (
  IN  UINT32       CmdId,
  OUT EFI_STATUS  *FakeStatus
  );

#endif /* FAKELOCK_OVERLAY_H_ */
