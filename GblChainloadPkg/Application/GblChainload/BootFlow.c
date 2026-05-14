/** @file BootFlow.c — chain-load orchestrator.

    Sequence:
      1. ResolveActiveAblName + AblUnwrap_LoadFromPartition
      2. DynamicPatchLib_EnsureInit + DynamicPatch_Apply (abort on mandatory miss)
      3. ProtocolHook_InstallAll (universal baseline + mode-N overlay; fail-closed)
      4. LoadImage + StartImage  (does not return on success)

    On any error (partition read fail / mandatory patch miss / hook install fail
    / LoadImage fail), return non-success — Entry.c falls through to FastbootLib.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>
#include <Library/GblLog.h>
#include <Library/LogFsLib.h>
#include <Library/PartitionTableUpdate.h>
#include <Library/AblUnwrapLib.h>
#include <Library/DynamicPatchLib.h>
#include <Library/ProtocolHookLib.h>

#ifndef GBL_MODE
# error "GBL_MODE must be defined"
#endif

#ifndef GBL_DEBUG
# define GBL_DEBUG 0
#endif

#if (GBL_DEBUG == 1)
# define SCR_PRINT(...)  Print (__VA_ARGS__)
#else
# define SCR_PRINT(...)  do {} while (0)
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

EFI_STATUS
EFIAPI
BootFlowChainLoad (VOID)
{
  EFI_STATUS           Status;
  CHAR16               AblName[MAX_GPT_NAME_SIZE];
  VOID                *Pe = NULL;
  UINT32               PeSize = 0;
  PATCH_RESULT         PatchRes = {0};
  EFI_HANDLE           ImageHandle = NULL;
#if (GBL_MODE >= 1)
  HOOK_INSTALL_RESULT  HookRes = {0};
#endif

  /* logfs was closed by EnterFastboot. Re-open here so BootFlow's per-step
     output (patch outcomes, hook install, transition) is persisted to logfs
     even if the chainload red-states before the kernel boots and populates
     /proc/bootloader_log. */
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
  SCR_PRINT (L"BootFlow: start (mode=%d)\n", (int)GBL_MODE);

  /* 1. Unwrap ABL PE from active slot. */
  Status = ResolveActiveAblName (AblName, MAX_GPT_NAME_SIZE);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BootFlow: slot resolve failed (%r)\n", Status));
    SCR_PRINT (L"BootFlow: slot resolve failed (%r)\n", Status);
    return Status;
  }

  Status = AblUnwrap_LoadFromPartition (AblName, &Pe, &PeSize);
  if (EFI_ERROR (Status)) {
    /* Some Qualcomm devices ship a single non-A/B `abl` partition. */
    GBL_INFO ("BootFlow: %s lookup failed (%r), trying 'abl'\n",
              AblName, Status);
    Status = AblUnwrap_LoadFromPartition (L"abl", &Pe, &PeSize);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "BootFlow: ABL not found (%r)\n", Status));
      SCR_PRINT (L"BootFlow: ABL not found (%r)\n", Status);
      return Status;
    }
  }
  GBL_INFO ("BootFlow: ABL loaded — %u bytes\n", PeSize);

  /* 2. Initialize patch table aggregator + apply patches. */
  DynamicPatchLib_EnsureInit ();
  DynamicPatch_Apply (Pe, PeSize, &PatchRes);

  GBL_INFO ("BootFlow: patches applied=%u missed=%u worst=%d\n",
            PatchRes.AppliedCount, PatchRes.MissedCount,
            (int)PatchRes.WorstOutcome);
  SCR_PRINT (L"BootFlow: patches applied=%u missed=%u worst=%d\n",
             PatchRes.AppliedCount, PatchRes.MissedCount,
             (int)PatchRes.WorstOutcome);

  if (PatchRes.WorstOutcome == PATCH_RESULT_MANDATORY_MISS) {
    DEBUG ((DEBUG_ERROR, "BootFlow: mandatory patch missed - aborting\n"));
    SCR_PRINT (L"BootFlow: mandatory patch missed - aborting\n");
    FreePool (Pe);
    return EFI_NOT_READY;
  }

  /* 3. Install protocol hooks (universal baseline + mode-N overlay).
        Mode-0 skips this entirely — no fakelock, no SCM/OplusSec drops. */
#if (GBL_MODE >= 1)
  Status = ProtocolHook_InstallAll (&HookRes);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BootFlow: hook install failed (%r) - aborting\n",
            Status));
    SCR_PRINT (L"BootFlow: hook install failed (%r) - aborting\n", Status);
    FreePool (Pe);
    return Status;
  }
#else
  GBL_INFO ("BootFlow: mode-0 — skipping ProtocolHook_InstallAll\n");
  SCR_PRINT (L"BootFlow: mode-0 -- skipping ProtocolHook_InstallAll\n");
#endif

  /* 4. LoadImage + StartImage. */

  /* Proper transition: release the logfs partition handle so the next EFI in
     the chain (the patched ABL or further-chained payloads) can mount it
     if they want.  Without this, the partition stays bound to our driver
     instance and ConnectController returns EFI_NOT_FOUND for the next caller. */
  GBL_INFO ("BootFlow: LogFs close before LoadImage\n");
  LogFsClose ();

  Status = gBS->LoadImage (FALSE, gImageHandle, NULL, Pe, PeSize, &ImageHandle);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BootFlow: LoadImage failed (%r)\n", Status));
    SCR_PRINT (L"BootFlow: LoadImage failed (%r)\n", Status);
    FreePool (Pe);
    return Status;
  }

  GBL_INFO ("BootFlow: handing off to patched ABL\n");
  SCR_PRINT (L"BootFlow: handing off to patched ABL\n");

  Status = gBS->StartImage (ImageHandle, NULL, NULL);

  /* StartImage rarely returns. */
  DEBUG ((DEBUG_WARN, "BootFlow: StartImage returned %r\n", Status));
  SCR_PRINT (L"BootFlow: StartImage returned %r\n", Status);
  if (ImageHandle != NULL) {
    gBS->UnloadImage (ImageHandle);
  }
  FreePool (Pe);
  return EFI_LOAD_ERROR;
}
