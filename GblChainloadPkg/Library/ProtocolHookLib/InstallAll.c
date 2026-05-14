/** @file InstallAll.c -- universal + per-mode hook dispatcher.

    Returns EFI_SUCCESS only if all required slot wrappers installed.
    On any error, caller must abort chain-load and fall through to FastbootLib.

    Universal-baseline policies (UniversalBaseline.c) live in the slot wrappers
    themselves -- they're called inline from HookedVBRwDeviceState etc.  This
    dispatcher ensures the slot wrappers themselves are installed for every
    mode, including mode-0 observation builds.

    Mode-1 overlay (Mode1Overlay.c) -- same pattern.  Future mode overlays
    must opt in explicitly; this dispatcher still installs the universal
    preservation baseline for every build mode.

    EbsHook is declared in HookCommon.h but not yet implemented; it is not
    called here until its source file lands.
**/
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/GblLog.h>

#include <Library/ProtocolHookLib.h>

#include "HookCommon.h"

EFI_STATUS
EFIAPI
ProtocolHook_InstallAll (
  OUT HOOK_INSTALL_RESULT  *Result
  )
{
  EFI_STATUS Status;

  if (Result == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  ZeroMem (Result, sizeof (*Result));

  /* 1. VerifiedBoot -- required.  Slot wrapper enforces universal
        write/reset swallow; mode-1 additionally mutates read/init state. */
  Status = InstallVerifiedBootHook ();
  if (EFI_ERROR (Status)) {
    Print (L"ProtocolHookLib: FATAL — VerifiedBoot install failed (%r), aborting chain-load\n",
           Status);
    return Status;
  }
  Result->VbInstalledSlots = 1;
  Result->VbExpectedSlots  = 1;

  /* 2. SCM -- required.  Universal TZ_BLOW_SW_FUSE drop. */
  Status = InstallScmHook ();
  if (EFI_ERROR (Status)) {
    Print (L"ProtocolHookLib: FATAL — SCM install failed (%r), aborting chain-load\n",
           Status);
    return Status;
  }
  Result->ScmInstalledSlots = 1;
  Result->ScmExpectedSlots  = 1;

  /* 3. Qseecom -- required.  Universal OplusSec 0x0A drop. */
  Status = InstallQseecomHook ();
  if (EFI_ERROR (Status)) {
    Print (L"ProtocolHookLib: FATAL — Qseecom install failed (%r), aborting chain-load\n",
           Status);
    return Status;
  }
  Result->QseecomInstalledSlots = 1;
  Result->QseecomExpectedSlots  = 1;

  /* 4. SPSS -- optional (observation-only).  Failure is logged but does
        not abort. */
  Status = InstallSpssHook ();
  if (EFI_ERROR (Status)) {
    Print (L"ProtocolHookLib: SPSS install failed (%r) - continuing (observation-only)\n",
           Status);
    Result->SpssInstalledSlots = 0;
  } else {
    Result->SpssInstalledSlots = 1;
  }
  Result->SpssExpectedSlots = 1;

  /* 5. BlockIo -- required for Oplus reserve preservation.  This hook
        observes partition reads/writes and swallows oplusreserve1 writes. */
  Status = InstallBlockIoHook ();
  if (EFI_ERROR (Status)) {
    Print (L"ProtocolHookLib: FATAL — BlockIo install failed (%r), aborting chain-load\n",
           Status);
    return Status;
  }
  Result->BlockIoInstalledSlots = 1;
  Result->BlockIoExpectedSlots  = 1;

  /* Aggregate -- all required hooks must be installed. */
  Result->UniversalRequiredOk =
    (Result->VbInstalledSlots    > 0 &&
     Result->ScmInstalledSlots   > 0 &&
     Result->QseecomInstalledSlots > 0 &&
     Result->BlockIoInstalledSlots > 0);

  if (!Result->UniversalRequiredOk) {
    Print (L"ProtocolHookLib: FATAL — universal baseline incomplete, aborting chain-load\n");
    return EFI_NOT_READY;
  }

  Result->ModeOverlayOk = TRUE;   /* Mode-specific overlays are inline/opt-in. */

  GBL_INFO (
    "ProtocolHookLib: installed (mode=%d,"
    " vb=%u/%u scm=%u/%u qsee=%u/%u spss=%u/%u blockio=%u/%u)\n",
    (int)GBL_MODE,
    Result->VbInstalledSlots,      Result->VbExpectedSlots,
    Result->ScmInstalledSlots,     Result->ScmExpectedSlots,
    Result->QseecomInstalledSlots, Result->QseecomExpectedSlots,
    Result->SpssInstalledSlots,    Result->SpssExpectedSlots,
    Result->BlockIoInstalledSlots, Result->BlockIoExpectedSlots
    );
  return EFI_SUCCESS;
}
