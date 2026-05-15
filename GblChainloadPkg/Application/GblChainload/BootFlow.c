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
#include <Library/UefiLib.h>      /* Print — fatal-path visibility */
#include <Library/DebugLib.h>
#include <Library/GblLog.h>
#include <Library/LogFsLib.h>
#include <Library/PartitionTableUpdate.h>
#include <Library/AblUnwrapLib.h>
#include <Library/DynamicPatchLib.h>
#include <Library/ProtocolHookLib.h>
#include <Library/CachedAblLib.h>

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
  HOOK_INSTALL_RESULT  HookRes = {0};

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

  /* 1. Prefer a build-time cached, already-patched ABL PE when present. The
        dynamic patch engine deliberately skips that payload to avoid applying
        binary rewrites twice. */
  CachedAbl_LogMetadata ();
  if (CachedAbl_IsPresent ()) {
    Status = CachedAbl_CopyPe (&Pe, &PeSize);
    if (EFI_ERROR (Status)) {
      Print (L"BootFlow: FATAL — cached ABL copy failed (%r)\n", Status);
      return Status;
    }
    GBL_INFO ("BootFlow: cached ABL PE loaded — %u bytes; dynamic patches skipped\n",
              PeSize);
  } else {
    /* Unwrap ABL PE from active slot. ResolveActiveAblName never fails
       in practice — slot suffix is derived from a static enum — so no
       error branch here. */
    (VOID)ResolveActiveAblName (AblName, MAX_GPT_NAME_SIZE);

    Status = AblUnwrap_LoadFromPartition (AblName, &Pe, &PeSize);
    if (EFI_ERROR (Status)) {
      /* Some Qualcomm devices ship a single non-A/B `abl` partition. */
      GBL_INFO ("BootFlow: %s lookup failed (%r), trying 'abl'\n",
                AblName, Status);
      Status = AblUnwrap_LoadFromPartition (L"abl", &Pe, &PeSize);
      if (EFI_ERROR (Status)) {
        Print (L"BootFlow: FATAL — ABL not found (%r)\n", Status);
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

    if (PatchRes.WorstOutcome == PATCH_RESULT_MANDATORY_MISS) {
      Print (L"BootFlow: FATAL — mandatory patch missed, aborting\n");
      FreePool (Pe);
      return EFI_NOT_READY;
    }
  }

  /* 3. Install protocol hooks (universal baseline + mode-N overlay).
        Mode-0 installs the universal observation/preservation hooks but no
        fakelock overlay. */
  Status = ProtocolHook_InstallAll (&HookRes);
  if (EFI_ERROR (Status)) {
    Print (L"BootFlow: FATAL — hook install failed (%r), aborting\n", Status);
    FreePool (Pe);
    return Status;
  }

  /* 4. LoadImage + StartImage. */

  /* Proper transition: release the logfs partition handle so the next EFI in
     the chain (the patched ABL or further-chained payloads) can mount it
     if they want.  Without this, the partition stays bound to our driver
     instance and ConnectController returns EFI_NOT_FOUND for the next caller. */
  GBL_INFO ("BootFlow: LogFs close before LoadImage\n");
  LogFsClose ();

  Status = gBS->LoadImage (FALSE, gImageHandle, NULL, Pe, PeSize, &ImageHandle);
  if (EFI_ERROR (Status)) {
    Print (L"BootFlow: FATAL — LoadImage failed (%r)\n", Status);
    FreePool (Pe);
    return Status;
  }

  GBL_INFO ("BootFlow: handing off to patched ABL\n");

  Status = gBS->StartImage (ImageHandle, NULL, NULL);

  /* StartImage rarely returns — when it does, the chain is broken. */
  Print (L"BootFlow: FATAL — StartImage returned %r\n", Status);
  if (ImageHandle != NULL) {
    gBS->UnloadImage (ImageHandle);
  }
  FreePool (Pe);
  return EFI_LOAD_ERROR;
}
