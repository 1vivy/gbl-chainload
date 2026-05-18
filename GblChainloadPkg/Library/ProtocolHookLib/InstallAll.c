/** @file InstallAll.c -- universal + per-mode hook dispatcher.

    Returns EFI_SUCCESS only if all required slot wrappers installed.
    On required errors, caller must abort chain-load and fall through to
    FastbootLib; optional observation-only hooks may fail open.

    Universal-baseline policies live in the slot wrappers themselves.  This
    dispatcher installs the wrappers needed for every mode: mode-0 gets
    observation plus the narrow preservation baseline; mode-1 layers its
    fakelock/persistence overlay on top.

    Mode-1 overlay (Mode1Overlay.c) -- same pattern.  Future mode overlays
    must opt in explicitly.

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

  /* 1. VerifiedBoot -- required for mode-1 fakelock/persistence overlay;
        optional observation-only wrapper in mode-0. */
  Status = InstallVerifiedBootHook ();
  if (EFI_ERROR (Status)) {
#if (GBL_MODE == 1)
    Print (L"ProtocolHookLib: FATAL — VerifiedBoot install failed (%r), aborting chain-load\n",
           Status);
    return Status;
#else
    Print (L"ProtocolHookLib: VerifiedBoot install failed (%r) - continuing (mode-0 observation-only)\n",
           Status);
    Result->VbInstalledSlots = 0;
#endif
  } else {
    Result->VbInstalledSlots = 1;
  }
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

  /* 3. Qseecom -- required for mode-1 OplusSec suppression; optional
        observation-only wrapper in mode-0. */
  Status = InstallQseecomHook ();
  if (EFI_ERROR (Status)) {
#if (GBL_MODE == 1) || (GBL_MODE == 2)
    Print (L"ProtocolHookLib: FATAL — Qseecom install failed (%r), aborting chain-load\n",
           Status);
    return Status;
#else
    Print (L"ProtocolHookLib: Qseecom install failed (%r) - continuing (mode-0 observation-only)\n",
           Status);
    Result->QseecomInstalledSlots = 0;
#endif
  } else {
    Result->QseecomInstalledSlots = 1;
  }
  Result->QseecomExpectedSlots  = 1;

  /* 4. SPSS -- required for mode-2 (KM/SPSS attestation overlay);
        optional (observation-only) for all other modes. */
  Status = InstallSpssHook ();
  if (EFI_ERROR (Status)) {
#if (GBL_MODE == 2)
    Print (L"ProtocolHookLib: FATAL — SPSS install failed (%r), aborting chain-load (mode-2)\n",
           Status);
    return Status;
#else
    Print (L"ProtocolHookLib: SPSS install failed (%r) - continuing (observation-only)\n",
           Status);
    Result->SpssInstalledSlots = 0;
#endif
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
    (Result->ScmInstalledSlots   > 0 &&
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
