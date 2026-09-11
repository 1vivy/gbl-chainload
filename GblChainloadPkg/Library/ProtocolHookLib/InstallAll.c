/** @file InstallAll.c -- universal + capability-gated hook dispatcher.

    Returns EFI_SUCCESS only if all required slot wrappers installed.
    Required-status for VerifiedBoot and Qseecom is derived from the
    runtime gManifest capability bits (WantFakelockHook, WantProfileSpoof).
    On required errors, caller must abort chain-load and fall through to
    FastbootLib; optional observation-only hooks may fail open. SPSS is
    always best-effort: the SPU keymint enforcement domain is dead on every
    target (PIL never loads on infiniti; protocol absent on macan), so its
    mirror is observation-only and its absence is never fatal.

    SCM and BlockIo are always required (safety baseline: SCM provides
    TZ_BLOW_SW_FUSE drop, BlockIo provides oplusreserve preservation).
    Universal-baseline policies live in the slot wrappers themselves.

    EbsHook is declared in HookCommon.h but not yet implemented; it is not
    called here until its source file lands.
**/
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/GblLog.h>
#include <Library/GblPayloadLib.h>

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

  /* 1. VerifiedBoot -- required iff fakelock-hook cap is set
        (fakelock/persistence overlay needs the VB slot mutators);
        otherwise optional observation-only wrapper. */
  Status = InstallVerifiedBootHook ();
  if (EFI_ERROR (Status)) {
    if (gManifest.WantFakelockHook) {
      Print (L"ProtocolHookLib: FATAL — VerifiedBoot install failed (%r), aborting chain-load\n",
             Status);
      return Status;
    }
    Print (L"ProtocolHookLib: VerifiedBoot install failed (%r) - continuing (observation-only)\n",
           Status);
    Result->VbInstalledSlots = 0;
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

  /* 3. Qseecom -- required iff either fakelock (OplusSec suppression) or
        profile-spoof (KM attestation overlay) cap is set; otherwise
        optional observation-only wrapper. */
  Status = InstallQseecomHook ();
  if (EFI_ERROR (Status)) {
    if (gManifest.WantFakelockHook || gManifest.WantProfileSpoof) {
      Print (L"ProtocolHookLib: FATAL — Qseecom install failed (%r), aborting chain-load\n",
             Status);
      return Status;
    }
    Print (L"ProtocolHookLib: Qseecom install failed (%r) - continuing (observation-only)\n",
           Status);
    Result->QseecomInstalledSlots = 0;
  } else {
    Result->QseecomInstalledSlots = 1;
  }
  Result->QseecomExpectedSlots  = 1;

  /* 4. SPSS -- always best-effort / observation-only. Install the
        ShareKeyMintInfo mutator when the protocol is published (keeps the SPU
        keymint mirror coherent under profile-spoof if a healthy SPU is up), but
        a failure to install is NEVER fatal — including under profile-spoof.

        Why this is not a half-spoof: on every device we target the SPU keymint
        *enforcement* domain is dead, so there is no second domain for a
        KM/QSEECOM-side spoof to be inconsistent with.
          - infiniti (validated): SPSS protocol publishes and this hook installs
            (spss=1/1), but the SPU PIL image never loads — the bootloader log
            shows `pil-SPSS Failed to load metadata` /
            `SPSSLib_LoadSPSS ProcessPilImageExt = Load Error`. The mirror is
            shared into a protocol whose backing SPU image is absent.
          - macan/sm8845: SPSS protocol is not published at all (NOT_FOUND); its
            SPU fails earlier still (PMIC init).
        In both cases the KM/QSEECOM spoof IS the whole spoof; refusing the boot
        because the dead SPU mirror is unhooked buys no security and would only
        block macan mode-2. So forward the miss to the log and continue. (If a
        device with a *live* SPU keymint domain ever turns up, revisit: there it
        would be a genuine half-spoof and a positive capability signal should
        gate strictness — but no such device is in evidence today.) */
  Status = InstallSpssHook ();
  if (EFI_ERROR (Status)) {
    GBL_INFO ("ProtocolHookLib: SPSS install failed (%r) - continuing "
              "(SPU keymint domain is dead on all targets; mirror is best-effort)\n",
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
    (Result->ScmInstalledSlots   > 0 &&
     Result->BlockIoInstalledSlots > 0);

  if (!Result->UniversalRequiredOk) {
    Print (L"ProtocolHookLib: FATAL — universal baseline incomplete, aborting chain-load\n");
    return EFI_NOT_READY;
  }

  Result->ModeOverlayOk = TRUE;   /* Mode-specific overlays are inline/opt-in. */

  GBL_INFO (
    "ProtocolHookLib: installed (fakelock=%u profile_spoof=%u,"
    " vb=%u/%u scm=%u/%u qsee=%u/%u spss=%u/%u blockio=%u/%u)\n",
    (UINT32)gManifest.WantFakelockHook,
    (UINT32)gManifest.WantProfileSpoof,
    Result->VbInstalledSlots,      Result->VbExpectedSlots,
    Result->ScmInstalledSlots,     Result->ScmExpectedSlots,
    Result->QseecomInstalledSlots, Result->QseecomExpectedSlots,
    Result->SpssInstalledSlots,    Result->SpssExpectedSlots,
    Result->BlockIoInstalledSlots, Result->BlockIoExpectedSlots
    );
  return EFI_SUCCESS;
}
