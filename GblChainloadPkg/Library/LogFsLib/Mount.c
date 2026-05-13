/** @file Mount.c — locate the `logfs` GPT-labeled partition, connect its
  block-IO controller, open the SimpleFileSystem, and stash the Root file
  handle for use by Rotation.c and PostGblLog.c.

  Ported from gbl_root_canoe LinuxLoader.c:277 (`MountLogFsForUefiLog`),
  with `#ifndef DISABLE_PRINT*` guards replaced by standard EDK2 `DEBUG`
  per REWRITE_PLAN.md §3.5.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/LinuxLoaderLib.h>
#include <Library/LogFsLib.h>
#include <Pi/PiFirmwareVolume.h>
#include <Protocol/FirmwareVolume2.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

/* Module-private state shared across Mount / Rotation / PostGblLog. */
EFI_FILE_PROTOCOL *gLogFsRoot = NULL;
EFI_FILE_PROTOCOL *gPostGblLog = NULL;
BOOLEAN            gLogFsReady = FALSE;

#define LOGFS_FLUSH_THRESHOLD  4096

STATIC UINTN       gLogFsDirtyBytes = 0;

/* EBS callback handle. logfs stays mounted across the chainload handoff so
 * the patched ABL's runtime DEBUG output (verbose-tier hook traces) can
 * still land in gbl-chainload_BootN.txt. We need a final flush at EBS
 * because the patched ABL never calls LogFsClose itself, and after EBS the
 * SimpleFS/Block-IO protocols backing gPostGblLog become invalid. */
STATIC EFI_EVENT   mLogFsEbsEvent = NULL;

STATIC VOID EFIAPI
LogFsExitBootServicesNotify (
  IN EFI_EVENT  Event,
  IN VOID      *Context
  )
{
  if (gLogFsReady && gPostGblLog != NULL) {
    gPostGblLog->Flush (gPostGblLog);
  }
  /* SimpleFS is going away — disarm all writers. We deliberately do NOT
   * call Close() here: protocol services are undefined at this TPL on
   * some implementations, and the OS-loader/kernel handoff cleans up
   * file handles regardless. */
  gLogFsReady      = FALSE;
  gLogFsDirtyBytes = 0;
}

/* Pre-sink mount-probe progress output. These lines fire before
 * LogFsInstallDebugSink runs, so they bypass any screen-mask filtering.
 * Gate them at compile time so production builds keep the screen quiet
 * during the happy-path mount; FAIL-case Print() calls below stay
 * unconditional so a real mount failure remains visible on every build. */
#ifndef GBL_DEBUG
# define GBL_DEBUG 0
#endif
#if (GBL_DEBUG == 1)
# define LOGFS_PROBE(...)  Print (__VA_ARGS__)
#else
# define LOGFS_PROBE(...)  do {} while (0)
#endif

/* Forward declarations (siblings provide). */
extern VOID LogFsRotateUefiLog (IN EFI_FILE_PROTOCOL *Root,
                                IN BOOLEAN            DeleteSource);
extern EFI_STATUS LogFsOpenPostGblLog (IN EFI_FILE_PROTOCOL *Root,
                                       OUT EFI_FILE_PROTOCOL **OutFile);

/* TRUE if our image was loaded by an FV-bearing parent (i.e. stock ABL
 * loaded us out of the gbl partition's FV). FALSE when running via
 * `fastboot stage` + `oem boot-efi` from a memory buffer.
 *
 * Used to gate destructive UefiLog rotation: the primary-GBL run may delete
 * UefiLog1.txt after archiving it. Staged payloads should still archive a
 * snapshot for extra bootchain/FastbootLib context, but must not delete the
 * source because the outer loader may still have it open. */
STATIC BOOLEAN
IsLoadedFromFv (VOID)
{
  EFI_LOADED_IMAGE_PROTOCOL     *Loaded = NULL;
  EFI_FIRMWARE_VOLUME2_PROTOCOL *Fv     = NULL;
  EFI_STATUS                     Status;

  if (gImageHandle == NULL) {
    return FALSE;
  }

  Status = gBS->HandleProtocol (gImageHandle, &gEfiLoadedImageProtocolGuid,
                                (VOID **)&Loaded);
  if (EFI_ERROR (Status) || Loaded == NULL || Loaded->DeviceHandle == NULL) {
    return FALSE;
  }

  Status = gBS->HandleProtocol (Loaded->DeviceHandle,
                                &gEfiFirmwareVolume2ProtocolGuid,
                                (VOID **)&Fv);
  return (!EFI_ERROR (Status) && Fv != NULL);
}

STATIC EFI_STATUS
MountLogFsRoot (
  OUT EFI_FILE_PROTOCOL **OutRoot
  )
{
  EFI_STATUS                       Status;
  PartiSelectFilter                HandleFilter;
  HandleInfo                       HandleInfoList[1];
  UINT32                           MaxHandles;
  UINT32                           BlkIoAttrib;
  EFI_HANDLE                      *Handle;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL               *Root;

  *OutRoot = NULL;

  ZeroMem (&HandleFilter, sizeof (HandleFilter));
  ZeroMem (HandleInfoList, sizeof (HandleInfoList));

  BlkIoAttrib = BLK_IO_SEL_PARTITIONED_GPT |
                BLK_IO_SEL_PARTITIONED_MBR |
                BLK_IO_SEL_MEDIA_TYPE_NON_REMOVABLE |
                BLK_IO_SEL_MATCH_PARTITION_LABEL;
  HandleFilter.PartitionLabel = L"logfs";
  MaxHandles = ARRAY_SIZE (HandleInfoList);

  LOGFS_PROBE (L"LogFs: [1/5] calling GetBlkIOHandles (label='logfs')\n");
  Status = GetBlkIOHandles (BlkIoAttrib, &HandleFilter,
                            HandleInfoList, &MaxHandles);
  LOGFS_PROBE (L"LogFs: [1/5] GetBlkIOHandles returned %r handles=%u\n",
               Status, MaxHandles);
  if (EFI_ERROR (Status) || MaxHandles != 1) {
    Print (L"LogFs: [1/5] FAIL — no logfs partition found (want 1 handle)\n");
    return EFI_NOT_FOUND;
  }

  Handle = HandleInfoList[0].Handle;
  if (Handle == NULL) {
    Print (L"LogFs: [1/5] FAIL — handle pointer is NULL\n");
    return EFI_NOT_FOUND;
  }
  LOGFS_PROBE (L"LogFs: [1/5] handle=%p OK\n", Handle);

  /* [2/5] Try HandleProtocol(SimpleFileSystem) directly first.
   * On canoe the platform BDS may have already connected FAT to the logfs
   * partition before we run.  If SimpleFileSystem is present we can skip
   * ConnectController entirely; if not, attempt ConnectController and then
   * re-probe.  EFI_NOT_FOUND from ConnectController means no driver bound
   * this time, but the protocol may still appear if the platform's FAT
   * driver is already managing the handle under a different context. */
  LOGFS_PROBE (L"LogFs: [2/5] probe SimpleFileSystem before ConnectController\n");
  Status = gBS->HandleProtocol (Handle,
                                &gEfiSimpleFileSystemProtocolGuid,
                                (VOID **)&Fs);
  LOGFS_PROBE (L"LogFs: [2/5] direct HandleProtocol(SimpleFS) returned %r\n", Status);
  if (EFI_ERROR (Status)) {
    /* Not yet connected — attempt ConnectController to trigger driver binding. */
    LOGFS_PROBE (L"LogFs: [2/5] calling gBS->ConnectController\n");
    Status = gBS->ConnectController (Handle, NULL, NULL, TRUE);
    LOGFS_PROBE (L"LogFs: [2/5] ConnectController returned %r\n", Status);
    /* EFI_NOT_FOUND = no driver bound right now but may still be available
     * via a pre-existing connection; EFI_ALREADY_STARTED = already bound.
     * Either way, proceed to the HandleProtocol probe below. */
    if (EFI_ERROR (Status) &&
        Status != EFI_ALREADY_STARTED &&
        Status != EFI_NOT_FOUND) {
      Print (L"LogFs: [2/5] FAIL — ConnectController unexpected error\n");
      return Status;
    }
    LOGFS_PROBE (L"LogFs: [2/5] ConnectController done (%r), re-probing SimpleFS\n", Status);
    Fs = NULL;
  }

  LOGFS_PROBE (L"LogFs: [3/5] calling gBS->HandleProtocol (SimpleFileSystem)\n");
  if (Fs == NULL) {
    Status = gBS->HandleProtocol (Handle,
                                  &gEfiSimpleFileSystemProtocolGuid,
                                  (VOID **)&Fs);
    LOGFS_PROBE (L"LogFs: [3/5] HandleProtocol(SimpleFS) returned %r\n", Status);
  } else {
    LOGFS_PROBE (L"LogFs: [3/5] SimpleFS already obtained in [2/5] probe\n");
    Status = EFI_SUCCESS;
  }
  if (EFI_ERROR (Status)) {
    Print (L"LogFs: [3/5] FAIL — SimpleFileSystem protocol unavailable\n");
    return Status;
  }

  LOGFS_PROBE (L"LogFs: [4/5] calling Fs->OpenVolume\n");
  Status = Fs->OpenVolume (Fs, &Root);
  LOGFS_PROBE (L"LogFs: [4/5] OpenVolume returned %r\n", Status);
  if (EFI_ERROR (Status)) {
    Print (L"LogFs: [4/5] FAIL — OpenVolume failed\n");
    return Status;
  }

  LOGFS_PROBE (L"LogFs: [5/5] mount succeeded — root=%p\n", Root);
  *OutRoot = Root;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
LogFsInit (VOID)
{
  EFI_STATUS Status;

  if (gLogFsReady) {
    return EFI_SUCCESS;
  }

  LOGFS_PROBE (L"LogFsLib: start\n");

  Status = MountLogFsRoot (&gLogFsRoot);
  if (EFI_ERROR (Status)) {
    Print (L"LogFsLib: finished — mount failed (%r)\n", Status);
    return Status;
  }

  /* Step 2: archive pre-GBL UefiLog1.txt → UefiLogSaved{0..4}.txt.
   * FV-loaded primary GBL rotates destructively so BDS starts a fresh log.
   * Staged payloads snapshot only, preserving the outer loader's open file. */
  if (IsLoadedFromFv ()) {
    DEBUG ((DEBUG_INFO,
            "LogFs: FV-loaded primary GBL — rotating UefiLog1.txt\n"));
    LogFsRotateUefiLog (gLogFsRoot, TRUE);
  } else {
    DEBUG ((DEBUG_INFO,
            "LogFs: staged context — snapshotting UefiLog1.txt\n"));
    LogFsRotateUefiLog (gLogFsRoot, FALSE);
  }

  Status = LogFsOpenPostGblLog (gLogFsRoot, &gPostGblLog);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "LogFs: post-GBL log open failed: %r\n", Status));
    LogFsClose ();
    Print (L"LogFsLib: finished — post-GBL log open failed (%r)\n", Status);
    return Status;
  }

  gLogFsReady = TRUE;

  /* Register the ExitBootServices auto-flush + disarm. Idempotent across
   * LogFsInit -> LogFsClose -> LogFsInit cycles in the same image (Entry.c
   * re-opens logfs from BootFlow). */
  if (mLogFsEbsEvent == NULL) {
    EFI_STATUS EvStatus = gBS->CreateEvent (
                                 EVT_SIGNAL_EXIT_BOOT_SERVICES,
                                 TPL_NOTIFY,
                                 LogFsExitBootServicesNotify,
                                 NULL,
                                 &mLogFsEbsEvent);
    if (EFI_ERROR (EvStatus)) {
      mLogFsEbsEvent = NULL;
      DEBUG ((DEBUG_WARN,
              "LogFs: EBS event registration failed (%r) — post-handoff "
              "verbose traces may not flush before SimpleFS goes away\n",
              EvStatus));
    }
  }

  LOGFS_PROBE (L"LogFsLib: finished\n");
  return EFI_SUCCESS;
}

BOOLEAN
EFIAPI
LogFsIsReady (VOID)
{
  return gLogFsReady;
}

EFI_STATUS
EFIAPI
LogFsClose (VOID)
{
  if (gPostGblLog != NULL) {
    gPostGblLog->Flush (gPostGblLog);
    gPostGblLog->Close (gPostGblLog);
    gPostGblLog = NULL;
  }
  if (gLogFsRoot != NULL) {
    gLogFsRoot->Close (gLogFsRoot);
    gLogFsRoot = NULL;
  }
  gLogFsReady = FALSE;
  gLogFsDirtyBytes = 0;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
LogFsFlush (VOID)
{
  if (!gLogFsReady || gPostGblLog == NULL) {
    return EFI_NOT_READY;
  }
  gLogFsDirtyBytes = 0;
  return gPostGblLog->Flush (gPostGblLog);
}

EFI_STATUS
EFIAPI
LogFsWrite (
  IN CONST CHAR8 *Buf,
  IN UINTN Len
  )
{
  EFI_STATUS Status;
  UINTN      Size;

  if (!gLogFsReady || gPostGblLog == NULL || Buf == NULL || Len == 0) {
    return EFI_NOT_READY;
  }

  Size   = Len;
  Status = gPostGblLog->Write (gPostGblLog, &Size, (VOID *)Buf);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  gLogFsDirtyBytes += Size;
  if (gLogFsDirtyBytes >= LOGFS_FLUSH_THRESHOLD) {
    gLogFsDirtyBytes = 0;
    return gPostGblLog->Flush (gPostGblLog);
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
LogFsWriteBlob (
  IN CONST CHAR16 *FileName,
  IN CONST VOID   *Data,
  IN UINTN         Len
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL *Blob = NULL;
  UINTN              Size;

  if (!gLogFsReady || gLogFsRoot == NULL ||
      FileName == NULL || Data == NULL || Len == 0) {
    return EFI_NOT_READY;
  }

  /* Delete first so existing longer blobs cannot leave stale tail bytes on
   * SimpleFS/FAT implementations that do not truncate on CREATE|WRITE. */
  Status = gLogFsRoot->Open (gLogFsRoot, &Blob, (CHAR16 *)FileName,
                             EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (!EFI_ERROR (Status) && Blob != NULL) {
    Blob->Delete (Blob);
    Blob = NULL;
  }

  Status = gLogFsRoot->Open (gLogFsRoot, &Blob, (CHAR16 *)FileName,
                             EFI_FILE_MODE_CREATE | EFI_FILE_MODE_WRITE |
                             EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status) || Blob == NULL) {
    return Status;
  }

  Size   = Len;
  Status = Blob->Write (Blob, &Size, (VOID *)Data);
  if (!EFI_ERROR (Status)) {
    Blob->Flush (Blob);
  }
  Blob->Close (Blob);
  return Status;
}
