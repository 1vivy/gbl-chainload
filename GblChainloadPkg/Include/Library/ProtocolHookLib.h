#ifndef PROTOCOL_HOOK_LIB_H_
#define PROTOCOL_HOOK_LIB_H_

#include <Uefi.h>

typedef struct {
  /* Slot installation results -- populated by ProtocolHook_InstallAll. */
  UINT32   VbInstalledSlots;
  UINT32   VbExpectedSlots;
  UINT32   ScmInstalledSlots;
  UINT32   ScmExpectedSlots;
  UINT32   QseecomInstalledSlots;
  UINT32   QseecomExpectedSlots;
  UINT32   SpssInstalledSlots;
  UINT32   SpssExpectedSlots;
  UINT32   BlockIoInstalledSlots;
  UINT32   BlockIoExpectedSlots;
  /* Aggregate flags. */
  BOOLEAN  UniversalRequiredOk;
  BOOLEAN  ModeOverlayOk;
} HOOK_INSTALL_RESULT;

/** Install universal baseline + per-mode overlay (selected by GBL_MODE feature
    flag).  Returns EFI_SUCCESS only if all required slots installed and the
    mode overlay configured cleanly.  Caller (BootFlow.c) aborts chain-load
    on any error. **/
EFI_STATUS
EFIAPI
ProtocolHook_InstallAll (
  OUT HOOK_INSTALL_RESULT  *Result
  );

#endif /* PROTOCOL_HOOK_LIB_H_ */
