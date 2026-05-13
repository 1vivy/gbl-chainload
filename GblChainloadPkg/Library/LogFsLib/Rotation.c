/** @file Rotation.c — archive UefiLog1.txt → UefiLogSaved{0..4}.txt across
  boots so BDS/fastboot bootchain history is preserved. FV-loaded primary GBL
  can delete UefiLog1.txt after archiving so BDS starts fresh; staged payloads
  only snapshot it because the outer loader may still have the file open.

  Ported from gbl_root_canoe LinuxLoader.c:362 (the second half of
  MountLogFsForUefiLog). DISABLE_PRINT* guards replaced with EDK2 DEBUG.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiLib.h>                 /* Print() */
#include <Protocol/SimpleFileSystem.h>

#define LOG_ROTATION_SLOTS  5
#define LOG_COPY_BUF_BYTES  4096

VOID
LogFsRotateUefiLog (
  IN EFI_FILE_PROTOCOL *Root,
  IN BOOLEAN            DeleteSource
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL *SourceFile = NULL;
  EFI_FILE_PROTOCOL *ArchiveFile = NULL;
  EFI_FILE_PROTOCOL *IndexFile = NULL;
  CHAR16             ArchivePath[32];
  UINT32             Slot = 0;
  UINT32             NextSlot;
  UINTN              Size;
  UINT8              Buffer[LOG_COPY_BUF_BYTES];
  UINT64             SourceMode;

  if (Root == NULL) {
    return;
  }

  /* Open existing UefiLog1.txt — if absent, nothing to rotate. Staged
   * payloads only snapshot it, so use read-only access to avoid colliding
   * with an outer loader that may still have the file open for append. */
  SourceMode = DeleteSource ?
                 (EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE) :
                 EFI_FILE_MODE_READ;
  Status = Root->Open (Root, &SourceFile, L"\\UefiLog1.txt",
                       SourceMode, 0);
  if (Status == EFI_NOT_FOUND) {
    DEBUG ((DEBUG_INFO, "LogFs rotate: no previous UefiLog1.txt to archive\n"));
    return;
  }
  if (EFI_ERROR (Status)) {
    /* Use Print() so the failure surfaces on the screen + UefiLog even
     * when DEBUG_WARN is filtered out by the runtime mask. This is the
     * only place we'd learn rotation broke. */
    Print (L"LogFs rotate: FAIL — open UefiLog1.txt: %r\n", Status);
    return;
  }

  /* Read/advance the slot index. The QcomModulePkg BDS reset boot counter
   * is unreliable, so we keep our own at \UefiLogSaved.idx. */
  Status = Root->Open (Root, &IndexFile, L"\\UefiLogSaved.idx",
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                         EFI_FILE_MODE_CREATE, 0);
  if (EFI_ERROR (Status)) {
    Print (L"LogFs rotate: FAIL — open UefiLogSaved.idx: %r\n", Status);
    SourceFile->Close (SourceFile);
    return;
  }

  Size = sizeof (Slot);
  Status = IndexFile->Read (IndexFile, &Size, &Slot);
  if (EFI_ERROR (Status) || Size != sizeof (Slot)) {
    Slot = 0;
  }
  Slot     %= LOG_ROTATION_SLOTS;
  NextSlot = (Slot + 1) % LOG_ROTATION_SLOTS;

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
    Print (L"LogFs rotate: FAIL — update slot index: %r\n", Status);
    SourceFile->Close (SourceFile);
    return;
  }

  /* Compose archive path and overwrite any existing slot. */
  UnicodeSPrint (ArchivePath, sizeof (ArchivePath),
                 L"\\UefiLogSaved%u.txt", Slot);

  Status = Root->Open (Root, &ArchiveFile, ArchivePath,
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  if (!EFI_ERROR (Status)) {
    /* Existing — delete to start fresh. */
    Status = ArchiveFile->Delete (ArchiveFile);
    ArchiveFile = NULL;
    if (EFI_ERROR (Status)) {
      Print (L"LogFs rotate: FAIL — delete old archive: %r\n", Status);
      SourceFile->Close (SourceFile);
      return;
    }
  }

  Status = Root->Open (Root, &ArchiveFile, ArchivePath,
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                         EFI_FILE_MODE_CREATE, 0);
  if (EFI_ERROR (Status)) {
    Print (L"LogFs rotate: FAIL — create archive: %r\n", Status);
    SourceFile->Close (SourceFile);
    return;
  }

  /* Copy SourceFile → ArchiveFile. */
  Status = SourceFile->SetPosition (SourceFile, 0);
  if (!EFI_ERROR (Status)) {
    Status = ArchiveFile->SetPosition (ArchiveFile, 0);
  }
  while (!EFI_ERROR (Status)) {
    Size   = sizeof (Buffer);
    Status = SourceFile->Read (SourceFile, &Size, Buffer);
    if (EFI_ERROR (Status) || Size == 0) {
      break;
    }
    Status = ArchiveFile->Write (ArchiveFile, &Size, Buffer);
  }

  if (!EFI_ERROR (Status)) {
    Status = ArchiveFile->Flush (ArchiveFile);
  }
  ArchiveFile->Close (ArchiveFile);

  if (EFI_ERROR (Status)) {
    Print (L"LogFs rotate: FAIL — archive copy: %r\n", Status);
    SourceFile->Close (SourceFile);
    return;
  }

  DEBUG ((DEBUG_INFO, "LogFs rotate: archived UefiLog1.txt to %s\n", ArchivePath));

  if (DeleteSource) {
    /* Delete the source so BDS will create a fresh UefiLog1.txt this boot. */
    Status = SourceFile->Delete (SourceFile);
    if (EFI_ERROR (Status)) {
      Print (L"LogFs rotate: FAIL — delete source UefiLog1.txt: %r\n", Status);
    }
  } else {
    SourceFile->Close (SourceFile);
  }
}
