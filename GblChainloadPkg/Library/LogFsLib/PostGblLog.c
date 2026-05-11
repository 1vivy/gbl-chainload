/** @file PostGblLog.c — open the clean post-GBL log file
  GblChainload_BootN.txt for the current boot, rotating across 5 slots
  with a private slot-index file (`GblChainloadSaved.idx`) so we don't
  collide with UefiLogSaved.idx.

  Step 2: write a startup banner. Step 3 wires the EDK2 DEBUG sink so
  all DEBUG()/Print() output post-GBL routes through here.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Protocol/SimpleFileSystem.h>

#define POST_GBL_SLOTS  5

#ifndef GBL_CHAINLOAD_VERSION
#define GBL_CHAINLOAD_VERSION  "unknown"
#endif

#ifndef GBL_BUILD_NAME
# define GBL_BUILD_NAME  "mode-unknown"
#endif

EFI_STATUS
LogFsOpenPostGblLog (
  IN  EFI_FILE_PROTOCOL  *Root,
  OUT EFI_FILE_PROTOCOL **OutFile
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL *IndexFile = NULL;
  EFI_FILE_PROTOCOL *LogFile = NULL;
  CHAR16             LogPath[32];
  UINT32             Slot = 0;
  UINT32             NextSlot;
  UINTN              Size;
  CHAR8              Banner[256];
  UINTN              BannerLen;

  if (Root == NULL || OutFile == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *OutFile = NULL;

  /* Slot index — separate from UefiLogSaved.idx so the two streams
   * rotate independently. */
  Status = Root->Open (Root, &IndexFile, L"\\GblChainloadSaved.idx",
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                         EFI_FILE_MODE_CREATE, 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "LogFs post-GBL: open idx failed: %r\n", Status));
    return Status;
  }

  Size   = sizeof (Slot);
  Status = IndexFile->Read (IndexFile, &Size, &Slot);
  if (EFI_ERROR (Status) || Size != sizeof (Slot)) {
    Slot = 0;
  }
  Slot     %= POST_GBL_SLOTS;
  NextSlot = (Slot + 1) % POST_GBL_SLOTS;

  Status = IndexFile->SetPosition (IndexFile, 0);
  if (!EFI_ERROR (Status)) {
    Size   = sizeof (NextSlot);
    Status = IndexFile->Write (IndexFile, &Size, &NextSlot);
  }
  if (!EFI_ERROR (Status)) {
    Status = IndexFile->Flush (IndexFile);
  }
  IndexFile->Close (IndexFile);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "LogFs post-GBL: idx update failed: %r\n", Status));
    return Status;
  }

  /* Compose path GblChainload_BootN.txt and clobber any prior contents. */
  UnicodeSPrint (LogPath, sizeof (LogPath),
                 L"\\GblChainload_Boot%u.txt", Slot);

  Status = Root->Open (Root, &LogFile, LogPath,
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (!EFI_ERROR (Status)) {
    Status = LogFile->Delete (LogFile);
    LogFile = NULL;
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "LogFs post-GBL: delete prior failed: %r\n", Status));
      return Status;
    }
  }

  Status = Root->Open (Root, &LogFile, LogPath,
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                         EFI_FILE_MODE_CREATE, 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "LogFs post-GBL: create %s failed: %r\n",
            LogPath, Status));
    return Status;
  }

  /* Banner identifies the build that wrote this log. */
  BannerLen = AsciiSPrint (Banner, sizeof (Banner),
                           "=== gbl-chainload %a - %a %a ===\n"
                           "=== boot slot %u of %u ===\n",
                           GBL_BUILD_NAME,
                           __DATE__, __TIME__,
                           Slot, POST_GBL_SLOTS);
  Size   = BannerLen;
  Status = LogFile->Write (LogFile, &Size, Banner);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "LogFs post-GBL: banner write failed: %r\n", Status));
    LogFile->Close (LogFile);
    return Status;
  }

  LogFile->Flush (LogFile);

  DEBUG ((DEBUG_INFO,
          "LogFs post-GBL: opened %s, slot %u/%u, banner %u bytes\n",
          LogPath, Slot, POST_GBL_SLOTS, (UINT32)BannerLen));

  *OutFile = LogFile;
  return EFI_SUCCESS;
}
