/** @file BlockIoHook.c — verbose EFI_BLOCK_IO_PROTOCOL observation and reserve write swallow.

  Hooks partition BlockIo ReadBlocks/WriteBlocks slots so every mode can
  preserve OPPO/OnePlus DeepTest state in `oplusreserve1` while still reporting
  success to ABL callers.  The hook also emits compact verbose read/write
  telemetry for partition-level BlockIo traffic; this is intentionally general
  knowledge for later manual scope expansion.
**/

#include <Uefi.h>
#include <Uefi/UefiGpt.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/GblLog.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>

#include "HookCommon.h"

#define BLOCK_IO_HOOK_MAX_RECORDS  192u
#define OPLUS_RESERVE1_TOKEN_LASTBLOCK_DELTA  0x3A5ULL
#define OPLUS_RESERVE1_UNLOCKRECORD_LASTBLOCK_DELTA  0x35CULL

extern EFI_GUID  gEfiPartitionRecordGuid;

typedef struct {
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  EFI_BLOCK_READ          OriginalReadBlocks;
  EFI_BLOCK_WRITE         OriginalWriteBlocks;
  EFI_LBA                 LastBlockAtInstall;
  UINT32                  BlockSizeAtInstall;
  CHAR16                  PartitionName[36];
  CHAR8                   PartitionNameAscii[37];
  BOOLEAN                 IsOplusReserve1;
  BOOLEAN                 Active;
} BLOCK_IO_HOOK_RECORD;

STATIC BLOCK_IO_HOOK_RECORD  gBlockIoRecords[BLOCK_IO_HOOK_MAX_RECORDS];
STATIC UINTN                 gBlockIoRecordCount = 0;

HOOK_REENTRY_DEFINE (gBlockIoGuard);

STATIC BOOLEAN
Char16EqualNoCase (
  IN CHAR16  A,
  IN CHAR16  B
  )
{
  if (A >= L'A' && A <= L'Z') {
    A = (CHAR16)(A | 0x20);
  }
  if (B >= L'A' && B <= L'Z') {
    B = (CHAR16)(B | 0x20);
  }
  return A == B;
}

/** Case-insensitive GPT-name match with the same permissive boundary rule used
    by AblUnwrapLib: Want must be a prefix and the next stored char must be NUL
    or space, or the GPT 36-char partition-name field is exhausted. **/
STATIC BOOLEAN
PartitionNameMatches (
  IN CONST CHAR16  *Stored,
  IN CONST CHAR16  *Want
  )
{
  UINTN Index;

  if (Stored == NULL || Want == NULL) {
    return FALSE;
  }

  for (Index = 0; Index < 36 && Want[Index] != L'\0'; Index++) {
    if (!Char16EqualNoCase (Stored[Index], Want[Index])) {
      return FALSE;
    }
  }

  if (Index >= 36) {
    return TRUE;
  }

  return Stored[Index] == L'\0' || Stored[Index] == L' ';
}

STATIC BOOLEAN
IsOplusReservePartitionName (
  IN CONST CHAR16  *Name
  )
{
  return PartitionNameMatches (Name, L"oplusreserve1") ||
         PartitionNameMatches (Name, L"opporeserve1");
}

STATIC VOID
CopyPartitionName36 (
  OUT CHAR16        *Dst,
  OUT CHAR8         *AsciiDst,
  IN  CONST CHAR16  *Src
  )
{
  UINTN Index;

  if (Dst == NULL || AsciiDst == NULL) {
    return;
  }

  for (Index = 0; Index < 36; Index++) {
    Dst[Index] = (Src == NULL) ? L'\0' : Src[Index];
    AsciiDst[Index] = (CHAR8)((Dst[Index] <= 0x7f) ? Dst[Index] : '?');
    if (Dst[Index] == L'\0') {
      break;
    }
  }
  if (Index >= 36) {
    Dst[35] = L'\0';
    AsciiDst[35] = '\0';
  } else {
    AsciiDst[Index] = '\0';
  }
}

STATIC BLOCK_IO_HOOK_RECORD *
FindRecordByBlockIo (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  UINTN Index;

  for (Index = 0; Index < gBlockIoRecordCount; Index++) {
    if (gBlockIoRecords[Index].Active && gBlockIoRecords[Index].BlockIo == This) {
      return &gBlockIoRecords[Index];
    }
  }

  return NULL;
}

STATIC UINT64
TransferBlockCount (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINTN                   BufferSize
  )
{
  UINT32 BlockSize;

  if (This == NULL || This->Media == NULL || This->Media->BlockSize == 0) {
    return 0;
  }

  BlockSize = This->Media->BlockSize;
  return (UINT64)((BufferSize + BlockSize - 1) / BlockSize);
}

STATIC BOOLEAN
BufferIsAllZero (
  IN CONST VOID  *Buffer,
  IN UINTN        BufferSize
  )
{
  CONST UINT8 *Bytes;
  UINTN        Index;

  if (Buffer == NULL || BufferSize == 0) {
    return FALSE;
  }

  Bytes = (CONST UINT8 *)Buffer;
  for (Index = 0; Index < BufferSize; Index++) {
    if (Bytes[Index] != 0) {
      return FALSE;
    }
  }
  return TRUE;
}

STATIC CONST CHAR8 *
ReserveWriteReason (
  IN CONST BLOCK_IO_HOOK_RECORD *Record,
  IN EFI_LBA                     Lba,
  IN UINTN                       BufferSize,
  IN CONST VOID                 *Buffer
  )
{
  EFI_LBA TokenLba;
  EFI_LBA UnlockRecordLba;

  if (Record == NULL) {
    return "reserve-write";
  }

  TokenLba        = Record->LastBlockAtInstall - OPLUS_RESERVE1_TOKEN_LASTBLOCK_DELTA;
  UnlockRecordLba = Record->LastBlockAtInstall - OPLUS_RESERVE1_UNLOCKRECORD_LASTBLOCK_DELTA;

  if (Lba == TokenLba && BufferIsAllZero (Buffer, BufferSize)) {
    return "token-zero-write";
  }
  if (Lba == TokenLba) {
    return "token-block-write";
  }
  if (Lba == UnlockRecordLba) {
    return "unlock-record-write";
  }
  return "reserve-write";
}

STATIC EFI_STATUS EFIAPI
HookedReadBlocks (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                  MediaId,
  IN  EFI_LBA                 Lba,
  IN  UINTN                   BufferSize,
  OUT VOID                   *Buffer
  )
{
  EFI_STATUS            Status;
  BLOCK_IO_HOOK_RECORD *Record;
  BOOLEAN               TopLevel;

  Record = FindRecordByBlockIo (This);
  if (Record == NULL || Record->OriginalReadBlocks == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  TopLevel = HookEnter (&gBlockIoGuard);
  Status = Record->OriginalReadBlocks (This, MediaId, Lba, BufferSize, Buffer);

  if (TopLevel) {
    VERBOSE ("blockio | op=read | p=%a | lba=%Lu | bytes=%u | blocks=%Lu | status=%r\n",
             Record->PartitionNameAscii,
             (UINT64)Lba,
             (UINT32)BufferSize,
             TransferBlockCount (This, BufferSize),
             Status);
  }
  HookLeave (&gBlockIoGuard);

  return Status;
}

STATIC EFI_STATUS EFIAPI
HookedWriteBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                  MediaId,
  IN EFI_LBA                 Lba,
  IN UINTN                   BufferSize,
  IN VOID                   *Buffer
  )
{
  EFI_STATUS            Status;
  BLOCK_IO_HOOK_RECORD *Record;
  CONST CHAR8          *Reason;
  BOOLEAN               TopLevel;

  Record = FindRecordByBlockIo (This);
  if (Record == NULL || Record->OriginalWriteBlocks == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  TopLevel = HookEnter (&gBlockIoGuard);

  if (Record->IsOplusReserve1) {
    if (TopLevel) {
      Reason = ReserveWriteReason (Record, Lba, BufferSize, Buffer);
      GBL_INFO ("blockio | op=write-swallow | reason=%a | p=%a | lba=%Lu | bytes=%u | blocks=%Lu | status=%r\n",
                Reason,
                Record->PartitionNameAscii,
                (UINT64)Lba,
                (UINT32)BufferSize,
                TransferBlockCount (This, BufferSize),
                EFI_SUCCESS);
      if (AsciiStrCmp (Reason, "token-zero-write") == 0) {
        Print (L"GBL: intercepted reserve token zeroing on %a LBA %Lu; token preserved\n",
               Record->PartitionNameAscii,
               (UINT64)Lba);
      }
    }
    HookLeave (&gBlockIoGuard);
    return EFI_SUCCESS;
  }

  Status = Record->OriginalWriteBlocks (This, MediaId, Lba, BufferSize, Buffer);

  if (TopLevel) {
    VERBOSE ("blockio | op=write | p=%a | lba=%Lu | bytes=%u | blocks=%Lu | status=%r\n",
             Record->PartitionNameAscii,
             (UINT64)Lba,
             (UINT32)BufferSize,
             TransferBlockCount (This, BufferSize),
             Status);
  }
  HookLeave (&gBlockIoGuard);

  return Status;
}

EFI_STATUS
InstallBlockIoHook (VOID)
{
  EFI_STATUS             Status;
  EFI_HANDLE            *Handles;
  UINTN                  HandleCount;
  UINTN                  Index;
  EFI_PARTITION_ENTRY   *PartEntry;
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  UINTN                  Installed;
  UINTN                  ReserveInstalled;
  UINTN                  Pass;
  BOOLEAN                IsReserve;

  Handles     = NULL;
  HandleCount = 0;
  Installed   = 0;
  ReserveInstalled = 0;

  if (gBlockIoRecordCount != 0) {
    return EFI_ALREADY_STARTED;
  }

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid,
                                    NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status) || Handles == NULL) {
    DEBUG ((DEBUG_ERROR, "BlockIoHook: LocateHandleBuffer failed: %r\n", Status));
    return EFI_NOT_FOUND;
  }

  for (Pass = 0; Pass < 2; Pass++) {
    for (Index = 0; Index < HandleCount; Index++) {
      PartEntry = NULL;
      BlockIo   = NULL;

      Status = gBS->HandleProtocol (Handles[Index], &gEfiPartitionRecordGuid,
                                    (VOID **)&PartEntry);
      if (EFI_ERROR (Status) || PartEntry == NULL) {
        continue;
      }

      IsReserve = IsOplusReservePartitionName (PartEntry->PartitionName);
      if ((Pass == 0 && !IsReserve) || (Pass == 1 && IsReserve)) {
        continue;
      }

      Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid,
                                    (VOID **)&BlockIo);
      if (EFI_ERROR (Status) || BlockIo == NULL) {
        continue;
      }

      if (BlockIo->ReadBlocks == HookedReadBlocks ||
          BlockIo->WriteBlocks == HookedWriteBlocks) {
        continue;
      }

      if (gBlockIoRecordCount >= BLOCK_IO_HOOK_MAX_RECORDS) {
        DEBUG ((DEBUG_WARN, "BlockIoHook: record table full at %u entries\n",
                (UINT32)gBlockIoRecordCount));
        break;
      }

      gBlockIoRecords[gBlockIoRecordCount].BlockIo              = BlockIo;
      gBlockIoRecords[gBlockIoRecordCount].OriginalReadBlocks   = BlockIo->ReadBlocks;
      gBlockIoRecords[gBlockIoRecordCount].OriginalWriteBlocks  = BlockIo->WriteBlocks;
      gBlockIoRecords[gBlockIoRecordCount].LastBlockAtInstall   = (BlockIo->Media != NULL) ? BlockIo->Media->LastBlock : 0;
      gBlockIoRecords[gBlockIoRecordCount].BlockSizeAtInstall   = (BlockIo->Media != NULL) ? BlockIo->Media->BlockSize : 0;
      gBlockIoRecords[gBlockIoRecordCount].IsOplusReserve1      = IsReserve;
      gBlockIoRecords[gBlockIoRecordCount].Active               = TRUE;
      CopyPartitionName36 (gBlockIoRecords[gBlockIoRecordCount].PartitionName,
                           gBlockIoRecords[gBlockIoRecordCount].PartitionNameAscii,
                           PartEntry->PartitionName);

      BlockIo->ReadBlocks  = HookedReadBlocks;
      BlockIo->WriteBlocks = HookedWriteBlocks;

      VERBOSE ("BlockIoHook: installed p=%a last=%Lu block=%u oplus=%u\n",
               gBlockIoRecords[gBlockIoRecordCount].PartitionNameAscii,
               (UINT64)gBlockIoRecords[gBlockIoRecordCount].LastBlockAtInstall,
               gBlockIoRecords[gBlockIoRecordCount].BlockSizeAtInstall,
               (UINT32)gBlockIoRecords[gBlockIoRecordCount].IsOplusReserve1);

      if (IsReserve) {
        ReserveInstalled++;
      }
      gBlockIoRecordCount++;
      Installed++;
    }

    if (Pass == 0 && ReserveInstalled == 0) {
      gBS->FreePool (Handles);
      DEBUG ((DEBUG_ERROR, "BlockIoHook: no oplus/oppo reserve1 BlockIo slot installed\n"));
      return EFI_NOT_FOUND;
    }
  }

  gBS->FreePool (Handles);

  if (Installed == 0) {
    DEBUG ((DEBUG_ERROR, "BlockIoHook: no partition BlockIo slots installed\n"));
    return EFI_NOT_FOUND;
  }

  GBL_INFO ("BlockIoHook: installed %u partition BlockIo records, reserve=%u\n",
            (UINT32)Installed, (UINT32)ReserveInstalled);
  return EFI_SUCCESS;
}

BOOLEAN
UninstallBlockIoHook (VOID)
{
  UINTN Index;
  BOOLEAN RestoredAll = TRUE;

  for (Index = 0; Index < gBlockIoRecordCount; Index++) {
    if (!gBlockIoRecords[Index].Active || gBlockIoRecords[Index].BlockIo == NULL) {
      continue;
    }

    if (gBlockIoRecords[Index].BlockIo->ReadBlocks == HookedReadBlocks) {
      gBlockIoRecords[Index].BlockIo->ReadBlocks = gBlockIoRecords[Index].OriginalReadBlocks;
    } else if (gBlockIoRecords[Index].OriginalReadBlocks != NULL &&
               gBlockIoRecords[Index].BlockIo->ReadBlocks != gBlockIoRecords[Index].OriginalReadBlocks) {
      RestoredAll = FALSE;
    }
    if (gBlockIoRecords[Index].BlockIo->WriteBlocks == HookedWriteBlocks) {
      gBlockIoRecords[Index].BlockIo->WriteBlocks = gBlockIoRecords[Index].OriginalWriteBlocks;
    } else if (gBlockIoRecords[Index].OriginalWriteBlocks != NULL &&
               gBlockIoRecords[Index].BlockIo->WriteBlocks != gBlockIoRecords[Index].OriginalWriteBlocks) {
      RestoredAll = FALSE;
    }
  }

  if (!RestoredAll) {
    GBL_INFO ("BlockIoHook: uninstall deferred; one or more slots no longer point at this wrapper\n");
    return FALSE;
  }

  if (gBlockIoRecordCount != 0) {
    GBL_INFO ("BlockIoHook: uninstalled %u records\n", (UINT32)gBlockIoRecordCount);
  }
  ZeroMem (gBlockIoRecords, sizeof (gBlockIoRecords));
  gBlockIoRecordCount = 0;
  return TRUE;
}
