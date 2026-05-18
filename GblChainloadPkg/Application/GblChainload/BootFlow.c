/** @file BootFlow.c — unified three-tier chain-load orchestrator.

    Tier 1: GblPayloadLib — cached ABL via GBLP1 overlay on EFISP.
    Tier 2: AblUnwrapLib + DynamicPatchLib — extract and patch live abl_<slot>.
    Tier 3: return EFI_NOT_FOUND — Entry.c falls through to EnterFastboot.

    Hook-lifecycle ordering (load-bearing per logfs_open_across_handoff):
      InstallAll → LogFsClose → LoadImage → StartImage

    On any error (partition read fail / mandatory patch miss / hook install
    fail / LoadImage fail), return non-success — Entry.c falls through to
    FastbootLib.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>      /* Print — fatal-path visibility */
#include <Library/DebugLib.h>
#include <Library/GblLog.h>
#include <Library/LogFsLib.h>
#include <Library/PartitionTableUpdate.h>
#include <Library/AblUnwrapLib.h>
#include <Library/DynamicPatchLib.h>
#include <Library/ProtocolHookLib.h>
#include <Library/GblPayloadLib.h>

#if (GBL_MODE == 2)
#include "../../Library/ProtocolHookLib/Mode2Overlay.h"
extern VOID GblFastbootSetMode2Warning (IN CONST CHAR8 *Warning);
#endif

#ifndef GBL_MODE
# error "GBL_MODE must be defined"
#endif

#ifndef GBL_DEBUG
# define GBL_DEBUG 0
#endif

/** Build the active abl partition name (L"abl_a" or L"abl_b") into Out. */
STATIC EFI_STATUS
ResolveActiveAblName (
  OUT CHAR16  *Out,
  IN  UINTN    OutCap
  )
{
  Slot Active = GetCurrentSlotSuffix ();

  StrnCpyS (Out, OutCap, L"abl", StrLen (L"abl"));
  StrnCatS (Out, OutCap, Active.Suffix, StrLen (Active.Suffix));
  return EFI_SUCCESS;
}

/**
  Tier 2: Extract the live abl_<slot> (or fallback "abl") PE from flash,
  then apply the dynamic patch table.  On mandatory-patch miss the function
  returns EFI_NOT_READY and the caller falls through to Tier 3.

  On success, *Pe is an AllocatePool buffer the caller must FreePool().
**/
STATIC EFI_STATUS
RunDynamicPatchOnSlotAbl (
  OUT VOID    **Pe,
  OUT UINT32   *PeSize
  )
{
  EFI_STATUS    Status;
  CHAR16        AblName[MAX_GPT_NAME_SIZE];
  PATCH_RESULT  PatchRes = {0};

  /* 2a. Unwrap ABL PE from active slot. */
  (VOID)ResolveActiveAblName (AblName, MAX_GPT_NAME_SIZE);

  Status = AblUnwrap_LoadFromPartition (AblName, Pe, PeSize);
  if (EFI_ERROR (Status)) {
    /* Some Qualcomm devices ship a single non-A/B `abl` partition. */
    GBL_INFO ("BootFlow: %s lookup failed (%r), trying 'abl'\n",
              AblName, Status);
    Status = AblUnwrap_LoadFromPartition (L"abl", Pe, PeSize);
    if (EFI_ERROR (Status)) {
      Print (L"BootFlow: FATAL — ABL not found (%r)\n", Status);
      return Status;
    }
  }
  GBL_INFO ("BootFlow: ABL loaded from flash — %u bytes\n", *PeSize);

  /* 2b. Initialize patch table aggregator + apply patches. */
  DynamicPatchLib_EnsureInit ();
  DynamicPatch_Apply (*Pe, *PeSize, &PatchRes);

  GBL_INFO ("BootFlow: patches applied=%u missed=%u worst=%d\n",
            PatchRes.AppliedCount, PatchRes.MissedCount,
            (int)PatchRes.WorstOutcome);

  if (PatchRes.WorstOutcome == PATCH_RESULT_MANDATORY_MISS) {
    Print (L"BootFlow: FATAL — mandatory patch missed, aborting\n");
    FreePool (*Pe);
    *Pe = NULL;
    return EFI_NOT_READY;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
BootFlowChainLoad (VOID)
{
  EFI_STATUS           Status;
  VOID                *Pe = NULL;
  UINT32               PeSize = 0;
  EFI_HANDLE           ImageHandle = NULL;
  HOOK_INSTALL_RESULT  HookRes = {0};
  CHAR8               *Origin = "<none>";

  /* logfs was closed by EnterFastboot. Re-open here so BootFlow's per-step
     output (tier selection, patch outcomes, hook install, transition) is
     persisted to logfs even if the chainload red-states before the kernel
     boots and populates /proc/bootloader_log. */
  {
    EFI_STATUS  LogStatus = LogFsInit ();
    if (!EFI_ERROR (LogStatus)) {
      GBL_INFO ("BootFlow: logfs re-opened for chainload session\n");
    } else {
      GBL_INFO ("BootFlow: logfs re-open failed (%r) - continuing without logfs\n",
                LogStatus);
    }
  }

  GBL_INFO ("BootFlow: start (mode=%d)\n", (int)GBL_MODE);

  GblPayload_LogProvenance (gImageHandle);

  /* Tier 1: cached ABL via GBLP1 overlay on EFISP. */
  Status = GblPayload_LoadCachedAbl (gImageHandle, &Pe, &PeSize);
  if (!EFI_ERROR (Status)) {
    Origin = "cached";
    GBL_INFO ("BootFlow: Tier 1 — loaded cached ABL (%u bytes)\n", PeSize);
  } else {
    GBL_INFO ("BootFlow: Tier 1 unavailable (%r), trying dynamic patch\n",
              Status);

    /* Tier 2: extract + patch live abl_<slot>. */
    Status = RunDynamicPatchOnSlotAbl (&Pe, &PeSize);
    if (EFI_ERROR (Status)) {
      GBL_INFO ("BootFlow: Tier 2 failed (%r), falling through to fastboot\n",
                Status);
      /* Tier 3: return — Entry.c → EnterFastboot. */
      return Status;
    }
    Origin = "dynamic";
  }

  GBL_INFO ("BootFlow: ABL loaded via %a (size=%u)\n", Origin, PeSize);

#if (GBL_MODE == 2)
  {
    struct gbl_mode2_profile Mode2Profile;
    EFI_STATUS M2Status =
        GblPayload_LoadMode2Profile (gImageHandle, &Mode2Profile);
    if (!EFI_ERROR (M2Status)) {
      Mode2_SetProfile (&Mode2Profile);
      GBL_INFO ("BootFlow: mode-2 profile loaded — spoof active\n");
    } else {
      GBL_INFO ("BootFlow: mode-2 profile unavailable (%r) — honest boot\n",
                M2Status);
      GblFastbootSetMode2Warning (
        (M2Status == EFI_NOT_FOUND)
          ? "MODE-2 PROFILE MISSING - booting honest, attestation will fail"
          : "MODE-2 PROFILE INVALID - booting honest, attestation will fail");
    }
  }
#endif

  /* Install protocol hooks (universal baseline + mode-N overlay).
     Mode-0 installs the universal observation/preservation hooks but no
     fakelock overlay.
     Ordering: InstallAll before LogFsClose (hooks may emit log lines). */
  Status = ProtocolHook_InstallAll (&HookRes);
  if (EFI_ERROR (Status)) {
    Print (L"BootFlow: FATAL — hook install failed (%r), aborting\n", Status);
    FreePool (Pe);
    return Status;
  }

  /* Proper transition: release the logfs partition handle so the next EFI
     in the chain (the patched ABL or further-chained payloads) can mount it
     if they want.  Without this, the partition stays bound to our driver
     instance and ConnectController returns EFI_NOT_FOUND for the next caller.
     Close AFTER hooks, BEFORE LoadImage (load-bearing per logfs_open_across_handoff). */
  GBL_INFO ("BootFlow: LogFs close before LoadImage\n");
  LogFsClose ();

  Status = gBS->LoadImage (FALSE, gImageHandle, NULL, Pe, PeSize, &ImageHandle);
  FreePool (Pe);
  Pe = NULL;
  if (EFI_ERROR (Status)) {
    Print (L"BootFlow: FATAL — LoadImage failed (%r)\n", Status);
    return Status;
  }

  GBL_INFO ("BootFlow: handing off to patched ABL (%a path)\n", Origin);

  Status = gBS->StartImage (ImageHandle, NULL, NULL);

  /* StartImage rarely returns — when it does, the chain is broken. */
  Print (L"BootFlow: FATAL — StartImage returned %r\n", Status);
  if (ImageHandle != NULL) {
    gBS->UnloadImage (ImageHandle);
  }
  return EFI_LOAD_ERROR;
}
