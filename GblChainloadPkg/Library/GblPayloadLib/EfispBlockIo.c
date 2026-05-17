/** @file EfispBlockIo.c
  Read the EFISP partition raw via BlockIO.  Mirrors LogFsLib's
  GetBlkIOHandles pattern (Mount.c) but skips the SimpleFileSystem step
  entirely — EFISP is not a FAT filesystem on supported targets; it holds
  a raw PE/FV image.

  The BlkIO handle returned by GetBlkIOHandles already carries a .BlkIo
  pointer, so no separate HandleProtocol call is needed.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/GblLog.h>
#include <Library/LinuxLoaderLib.h>

/* Read up to 16 MiB starting at the partition's first LBA.  The header
   parser walks PE end -> GBLP1 magic; we don't need more than that.
   Matches GBLP1_TOTAL_SIZE_CAP in tools/shared/gblp1.h. */
#define EFISP_READ_CAP  (16u * 1024u * 1024u)

/**
  Read raw bytes from the EFISP partition via BlockIO.

  On success the caller owns *OutBytes and must FreePool() it.

  @param[out] OutBytes   Allocated buffer containing partition data.
  @param[out] OutSize    Number of valid bytes in *OutBytes.

  @retval EFI_SUCCESS         Data read into *OutBytes / *OutSize.
  @retval EFI_NOT_FOUND       No single EFISP handle located.
  @retval EFI_OUT_OF_RESOURCES AllocatePool failed.
  @retval other               BlockIO ReadBlocks error.
**/
EFI_STATUS
ReadEfispRawBytes (
  OUT VOID  **OutBytes,
  OUT UINTN  *OutSize
  )
{
  EFI_STATUS          Status;
  HandleInfo          HandleInfoList[2];
  UINT32              MaxHandles;
  PartiSelectFilter   HandleFilter;
  UINT32              BlkIoAttrib;
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  UINT32              BlockSize;
  UINTN               PartitionSize;
  UINTN               ReadSize;
  VOID               *Buf;

  BlkIoAttrib = BLK_IO_SEL_PARTITIONED_GPT      |
                BLK_IO_SEL_PARTITIONED_MBR       |
                BLK_IO_SEL_MEDIA_TYPE_NON_REMOVABLE |
                BLK_IO_SEL_MATCH_PARTITION_LABEL;

  ZeroMem (&HandleFilter,   sizeof (HandleFilter));
  ZeroMem (HandleInfoList,  sizeof (HandleInfoList));

  HandleFilter.PartitionLabel = L"efisp";
  MaxHandles = ARRAY_SIZE (HandleInfoList);

  GBL_INFO ("gbl-payload: GetBlkIOHandles(efisp) start\n");
  Status = GetBlkIOHandles (BlkIoAttrib, &HandleFilter,
                            HandleInfoList, &MaxHandles);
  GBL_INFO ("gbl-payload: GetBlkIOHandles returned %r handles=%u\n",
            Status, MaxHandles);
  if (EFI_ERROR (Status) || MaxHandles != 1) {
    GBL_INFO ("gbl-payload: efisp partition not found (want 1 handle)\n");
    return EFI_NOT_FOUND;
  }

  /* BlkIo is populated by GetBlkIOHandles for all filter types. */
  BlockIo = HandleInfoList[0].BlkIo;
  if (BlockIo == NULL) {
    GBL_INFO ("gbl-payload: HandleInfo.BlkIo is NULL\n");
    return EFI_NOT_FOUND;
  }

  BlockSize     = BlockIo->Media->BlockSize;
  PartitionSize = (UINTN)(BlockIo->Media->LastBlock + 1) * BlockSize;
  ReadSize      = PartitionSize > EFISP_READ_CAP ? EFISP_READ_CAP
                                                  : PartitionSize;
  /* Round down to block boundary. */
  ReadSize = (ReadSize / BlockSize) * BlockSize;
  if (ReadSize == 0) {
    GBL_INFO ("gbl-payload: efisp effective read size is 0\n");
    return EFI_NOT_FOUND;
  }

  Buf = AllocatePool (ReadSize);
  if (Buf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId,
                                0, ReadSize, Buf);
  if (EFI_ERROR (Status)) {
    FreePool (Buf);
    GBL_INFO ("gbl-payload: ReadBlocks status=%r\n", Status);
    return Status;
  }

  *OutBytes = Buf;
  *OutSize  = ReadSize;
  return EFI_SUCCESS;
}
