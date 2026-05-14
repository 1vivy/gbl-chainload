/** @file InstallAll.c -- universal + per-mode hook dispatcher.

    Returns EFI_SUCCESS only if all required slot wrappers installed.
    On any error, caller must abort chain-load and fall through to FastbootLib.

    Universal-baseline policies (UniversalBaseline.c) live in the slot wrappers
    themselves -- they're called inline from HookedVBRwDeviceState etc.  This
    dispatcher just ensures the slot wrappers themselves are installed.

    Mode-1 overlay (Mode1Overlay.c) -- same pattern.  Mode-2/3 overlays
    are TBD.

    EbsHook is declared in HookCommon.h but not yet implemented; it is not
    called here until its source file lands.
**/
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/GblLog.h>

#include <Library/ProtocolHookLib.h>

#if (GBL_MODE == 0)

EFI_STATUS
EFIAPI
ProtocolHook_InstallAll (
  OUT HOOK_INSTALL_RESULT  *Result
  )
{
  /* Mode-0 -- no protocol hooks installed.  BootFlow.c never calls this
     in mode-0, but the symbol must still link.  Return EFI_SUCCESS as a
     defensive default. */
  if (Result != NULL) {
    ZeroMem (Result, sizeof (*Result));
    Result->UniversalRequiredOk = TRUE;
    Result->ModeOverlayOk       = TRUE;
  }
  return EFI_SUCCESS;
}

#elif (GBL_MODE == 1)

/* Existing slot installers (defined in their respective .c files). */
EFI_STATUS InstallVerifiedBootHook (VOID);
EFI_STATUS InstallScmHook          (VOID);
EFI_STATUS InstallQseecomHook      (VOID);
EFI_STATUS InstallSpssHook         (VOID);

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
        write/reset swallow + mode-1 read/init mutate. */
  Status = InstallVerifiedBootHook ();
  if (EFI_ERROR (Status)) {
    Print (L"ProtocolHookLib: VerifiedBoot install failed (%r) - abort chain-load\n",
           Status);
    return Status;
  }
  Result->VbInstalledSlots = 1;
  Result->VbExpectedSlots  = 1;

  /* 2. SCM -- required.  Universal TZ_BLOW_SW_FUSE drop. */
  Status = InstallScmHook ();
  if (EFI_ERROR (Status)) {
    Print (L"ProtocolHookLib: SCM install failed (%r) - abort chain-load\n",
           Status);
    return Status;
  }
  Result->ScmInstalledSlots = 1;
  Result->ScmExpectedSlots  = 1;

  /* 3. Qseecom -- required.  Universal OplusSec 0x0A drop. */
  Status = InstallQseecomHook ();
  if (EFI_ERROR (Status)) {
    Print (L"ProtocolHookLib: Qseecom install failed (%r) - abort chain-load\n",
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

  /* Aggregate -- all three required hooks must be installed. */
  Result->UniversalRequiredOk =
    (Result->VbInstalledSlots    > 0 &&
     Result->ScmInstalledSlots   > 0 &&
     Result->QseecomInstalledSlots > 0);

  if (!Result->UniversalRequiredOk) {
    Print (L"ProtocolHookLib: universal baseline incomplete - abort chain-load\n");
    return EFI_NOT_READY;
  }

  Result->ModeOverlayOk = TRUE;   /* Mode-1 overlay inline; mode-2/3 overlays TBD. */

  GBL_INFO (
    "ProtocolHookLib: installed (mode=%d,"
    " vb=%u/%u scm=%u/%u qsee=%u/%u spss=%u/%u)\n",
    (int)GBL_MODE,
    Result->VbInstalledSlots,      Result->VbExpectedSlots,
    Result->ScmInstalledSlots,     Result->ScmExpectedSlots,
    Result->QseecomInstalledSlots, Result->QseecomExpectedSlots,
    Result->SpssInstalledSlots,    Result->SpssExpectedSlots
    );
  return EFI_SUCCESS;
}

#else
# error "GBL_MODE must be 0 or 1"
#endif
