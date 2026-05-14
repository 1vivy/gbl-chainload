/** @file AblUnwrapLib.c
  Read GPT partition → FV header → FFS files → PE32 section. Section
  walker handles raw PE32/TE plus the wrapper section types canoe ABL
  actually uses: EFI_SECTION_COMPRESSION (NOT_COMPRESSED + LZMA),
  EFI_SECTION_GUID_DEFINED (LZMA-GUID + unknown-GUID pass-through),
  and EFI_SECTION_FIRMWARE_VOLUME_IMAGE (nested FV).
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/GblLog.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/LinuxLoaderLib.h>
#include <Library/PartitionTableUpdate.h>
#include <Library/AblUnwrapLib.h>
#include <Pi/PiFirmwareVolume.h>
#include <Pi/PiFirmwareFile.h>
#include <Uefi/UefiGpt.h>
#include <Protocol/BlockIo.h>

/* LzmaCustomDecompressLib (MdeModulePkg) — public surface declared
 * locally to avoid relying on the package's internal include path. */
EFI_STATUS EFIAPI LzmaUefiDecompressGetInfo (
  IN  CONST VOID  *Source,
  IN  UINT32       SourceSize,
  OUT UINT32      *DestinationSize,
  OUT UINT32      *ScratchSize
  );

EFI_STATUS EFIAPI LzmaUefiDecompress (
  IN CONST VOID  *Source,
  IN UINTN        SourceSize,
  IN OUT VOID    *Destination,
  IN OUT VOID    *Scratch
  );

/* GUID for LZMA-compressed EFI_SECTION_GUID_DEFINED sections — matches
 * EFI_LZMA_CUSTOM_DECOMPRESS_GUID in MdeModulePkg/Include/Guid/LzmaDecompress.h. */
STATIC EFI_GUID  mLzmaGuid = {
  0xEE4E5898, 0x3914, 0x4259,
  { 0x9D, 0x6E, 0xDC, 0x7B, 0xD7, 0x94, 0x03, 0xCF }
};

/* Forward declarations. */
STATIC EFI_STATUS GetHandle (CHAR16 *Name, EFI_BLOCK_IO_PROTOCOL **Out);
STATIC EFI_STATUS ReadEntirePartition (CHAR16 *Name, VOID **Buf, UINTN *Size);
STATIC UINT8 *FindFirmwareVolume (UINT8 *Data, UINTN Size, UINTN *OutFvSize);
STATIC BOOLEAN FindPe32InFv (UINT8 *FvBuf, UINTN FvSize, UINT8 **PeOut, UINTN *PeSizeOut);
STATIC BOOLEAN FindPe32InSectionStream (UINT8 *Buf, UINTN BufSize, UINT8 **PeOut, UINTN *PeSizeOut);
STATIC BOOLEAN ScanAndFindPe32 (UINT8 *Buf, UINTN BufSize, UINT8 **PeOut, UINTN *PeSizeOut);

STATIC UINT16
ReadLe16 (
  IN CONST UINT8 *P
  )
{
  return (UINT16)((UINT16)P[0] | ((UINT16)P[1] << 8));
}

STATIC UINT32
ReadLe24 (
  IN CONST UINT8 *P
  )
{
  return (UINT32)P[0] | ((UINT32)P[1] << 8) | ((UINT32)P[2] << 16);
}

STATIC UINT32
ReadLe32 (
  IN CONST UINT8 *P
  )
{
  return (UINT32)P[0] | ((UINT32)P[1] << 8) |
         ((UINT32)P[2] << 16) | ((UINT32)P[3] << 24);
}

STATIC UINT64
ReadLe64 (
  IN CONST UINT8 *P
  )
{
  return (UINT64)ReadLe32 (P) | ((UINT64)ReadLe32 (P + 4) << 32);
}

/** Case-insensitive length-tolerant partition-name match.
 *
 *  Matches if `Want` is a case-insensitive prefix of `Stored` AND the
 *  next `Stored` char is a string-end indicator (null / space / past
 *  the 36-char EFI_PARTITION_NAME_LEN). Some Qcom GPT writers store
 *  partition names with non-printable trailing bytes or different case
 *  than expected — be permissive on the boundary.
 */
STATIC BOOLEAN
PartitionNameMatches (
  IN CONST CHAR16 *Stored,
  IN CONST CHAR16 *Want
  )
{
  UINTN i;
  if (Stored == NULL || Want == NULL) {
    return FALSE;
  }
  for (i = 0; i < 36 && Want[i] != L'\0'; i++) {
    CHAR16 a = (Stored[i] >= L'A' && Stored[i] <= L'Z') ?
               (CHAR16)(Stored[i] | 0x20) : Stored[i];
    CHAR16 b = (Want[i]   >= L'A' && Want[i]   <= L'Z') ?
               (CHAR16)(Want[i]   | 0x20) : Want[i];
    if (a != b) {
      return FALSE;
    }
  }
  if (i >= 36) return TRUE;
  return Stored[i] == L'\0' || Stored[i] == L' ';
}

STATIC EFI_STATUS
GetHandle (
  IN  CHAR16                  *PartitionName,
  OUT EFI_BLOCK_IO_PROTOCOL  **PartHandle
  )
{
  EFI_STATUS            Status;
  EFI_HANDLE           *Handles    = NULL;
  UINTN                 HandleCount = 0;
  UINTN                 i;
  EFI_PARTITION_ENTRY  *PartEntry;
  EFI_BLOCK_IO_PROTOCOL *BlkIo;

  *PartHandle = NULL;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid,
                                    NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status) || Handles == NULL) {
    DEBUG ((DEBUG_WARN, "AblUnwrap: LocateHandleBuffer failed: %r\n", Status));
    return EFI_NOT_FOUND;
  }

  UINT32     PartIdx     = 0;
  EFI_HANDLE FoundHandle = NULL;

  for (i = 0; i < HandleCount; i++) {
    PartEntry = NULL;
    Status = gBS->HandleProtocol (Handles[i], &gEfiPartitionRecordGuid,
                                  (VOID **)&PartEntry);
    if (EFI_ERROR (Status) || PartEntry == NULL) {
      continue;
    }
    PartIdx++;

    if (PartitionNameMatches (PartEntry->PartitionName, PartitionName)) {
      FoundHandle = Handles[i];
      break;
    }
  }

  if (FoundHandle != NULL) {
    BlkIo = NULL;
    Status = gBS->HandleProtocol (FoundHandle, &gEfiBlockIoProtocolGuid,
                                  (VOID **)&BlkIo);
    if (EFI_ERROR (Status) || BlkIo == NULL) {
      Print (L"AblUnwrap: name matched but BlockIo HandleProtocol failed: %r\n",
             Status);
    } else {
      *PartHandle = BlkIo;
      FreePool (Handles);
      Print (L"AblUnwrap: matched %s\n", PartitionName);
      return EFI_SUCCESS;
    }
  }

  FreePool (Handles);
  DEBUG ((DEBUG_WARN, "AblUnwrap: partition not found: %s (scanned %u handles)\n",
          PartitionName, (UINT32)HandleCount));
  return EFI_NOT_FOUND;
}

STATIC EFI_STATUS
ReadEntirePartition (
  IN  CHAR16  *PartitionName,
  OUT VOID   **Buffer,
  OUT UINTN   *BufferSize
  )
{
  EFI_STATUS             Status;
  EFI_BLOCK_IO_PROTOCOL *BlkIo = NULL;
  UINTN                  Size;
  VOID                  *Buf;

  Status = GetHandle (PartitionName, &BlkIo);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!BlkIo->Media->MediaPresent) {
    DEBUG ((DEBUG_WARN, "AblUnwrap: no media on %s\n", PartitionName));
    return EFI_NO_MEDIA;
  }

  Size = (UINTN)BlkIo->Media->BlockSize *
         (UINTN)(BlkIo->Media->LastBlock + 1);

  Buf = AllocatePool (Size);
  if (Buf == NULL) {
    DEBUG ((DEBUG_ERROR, "AblUnwrap: AllocatePool %lu bytes failed\n",
            (UINT64)Size));
    return EFI_OUT_OF_RESOURCES;
  }

  Status = BlkIo->ReadBlocks (BlkIo, BlkIo->Media->MediaId, 0, Size, Buf);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "AblUnwrap: ReadBlocks failed: %r\n", Status));
    FreePool (Buf);
    return Status;
  }

  *Buffer     = Buf;
  *BufferSize = Size;
  return EFI_SUCCESS;
}

STATIC UINT8 *
FindFirmwareVolume (
  IN  UINT8  *Data,
  IN  UINTN   Size,
  OUT UINTN  *OutFvSize
  )
{
  UINTN i;

  if (Data == NULL || Size < sizeof (EFI_FIRMWARE_VOLUME_HEADER)) {
    return NULL;
  }

  for (i = 0; i + sizeof (EFI_FIRMWARE_VOLUME_HEADER) <= Size; i++) {
    EFI_FIRMWARE_VOLUME_HEADER *FvH =
        (EFI_FIRMWARE_VOLUME_HEADER *)(Data + i);
    if (FvH->Signature == EFI_FVH_SIGNATURE &&
        FvH->FvLength  >  0 &&
        FvH->FvLength  <= (UINT64)(Size - i)) {
      *OutFvSize = (UINTN)FvH->FvLength;
      return Data + i;
    }
  }
  return NULL;
}

STATIC BOOLEAN
GetSectionSizeEx (
  IN  UINT8  *SecBase,
  IN  UINTN   Remaining,
  OUT UINTN  *Size,
  OUT UINTN  *HdrSize
  )
{
  UINTN S;

  if (SecBase == NULL || Size == NULL || HdrSize == NULL || Remaining < 4) {
    return FALSE;
  }

  S = (UINTN)ReadLe24 (SecBase);
  if (S == 0xFFFFFF) {
    if (Remaining < 8) {
      return FALSE;
    }
    *HdrSize = 8;
    *Size = (UINTN)ReadLe32 (SecBase + 4);
    return TRUE;
  }
  *HdrSize = 4;
  *Size = S;
  return TRUE;
}

STATIC BOOLEAN
GetFfsSizeEx (
  IN  UINT8  *FfsBase,
  IN  UINTN   Remaining,
  OUT UINTN  *Size,
  OUT UINTN  *HdrSize
  )
{
  UINT8 Attrs;
  UINTN S;

  if (FfsBase == NULL || Size == NULL || HdrSize == NULL || Remaining < 24) {
    return FALSE;
  }

  Attrs = FfsBase[19];
  S = (UINTN)ReadLe24 (FfsBase + 20);

  if (S == 0xFFFFFF && (Attrs & 0x01)) {
    if (Remaining < 32) {
      return FALSE;
    }
    *HdrSize = 32;
    *Size = (UINTN)ReadLe64 (FfsBase + 24);
    return TRUE;
  }
  *HdrSize = 24;
  *Size = S;
  return TRUE;
}

/* Recursive FFS-section walker. Handles raw PE32/TE plus the wrapper
 * section types canoe ABL actually uses: EFI_SECTION_COMPRESSION
 * (with EFI_NOT_COMPRESSED and EFI_STANDARD_COMPRESSION/LZMA),
 * EFI_SECTION_GUID_DEFINED (with the LZMA GUID and unknown-GUID
 * pass-through), and EFI_SECTION_FIRMWARE_VOLUME_IMAGE (nested FV).
 *
 * Mirrors gbl_root_canoe LinuxLoader.c FindPe32InSectionStream so we
 * stay within a known-good shape rather than rolling our own. */
STATIC BOOLEAN
FindPe32InSectionStream (
  IN  UINT8   *Buf,
  IN  UINTN    BufSize,
  OUT UINT8  **PeOut,
  OUT UINTN   *PeSizeOut
  )
{
  UINTN Offset = 0;

  while (Offset + 4 <= BufSize) {
    UINTN  SecHdrSize = 0;
    UINTN  SecSize;
    UINT8  SecType;
    UINT8 *SecData;
    UINTN  SecDataSize;

    Offset = (Offset + 3) & ~(UINTN)3;
    if (Offset + 4 > BufSize) {
      break;
    }

    if (!GetSectionSizeEx (Buf + Offset, BufSize - Offset,
                           &SecSize, &SecHdrSize)) {
      DEBUG ((DEBUG_WARN, "AblUnwrap: invalid/truncated section header\n"));
      break;
    }
    SecType = Buf[Offset + 3];

    VERBOSE ("AblUnwrap: section @ 0x%x type=0x%02x size=0x%x hdr=%u\n",
             (UINT32)Offset, (UINT32)SecType,
             (UINT32)SecSize, (UINT32)SecHdrSize);

    if (SecSize < SecHdrSize || SecSize == 0 ||
        SecSize > BufSize - Offset) {
      DEBUG ((DEBUG_WARN, "AblUnwrap: invalid section size, stop\n"));
      break;
    }

    SecData     = Buf + Offset + SecHdrSize;
    SecDataSize = SecSize - SecHdrSize;

    switch (SecType) {

    case EFI_SECTION_PE32:
    case EFI_SECTION_TE:
    {
      UINT8 *Copy = AllocatePool (SecDataSize);
      if (Copy == NULL) {
        return FALSE;
      }
      CopyMem (Copy, SecData, SecDataSize);
      *PeOut     = Copy;
      *PeSizeOut = SecDataSize;
      Print (L"AblUnwrap: found PE/TE %u bytes\n", (UINT32)SecDataSize);
      return TRUE;
    }

    case EFI_SECTION_COMPRESSION:
    {
      if (SecDataSize < 5) {
        break;
      }
      UINT8  CompType  = SecData[4];
      UINT8 *CompData  = SecData + 5;
      UINTN  CompLen   = SecDataSize - 5;

      VERBOSE ("AblUnwrap: COMPRESSION type=0x%02x uncomp=%u comp=%u\n",
               (UINT32)CompType, ReadLe32 (SecData), (UINT32)CompLen);

      if (CompType == EFI_NOT_COMPRESSED) {
        UINTN CompDataOff     = (UINTN)(CompData - Buf);
        UINTN CompDataOffAlgn = (CompDataOff + 3) & ~(UINTN)3;
        UINTN Skip            = CompDataOffAlgn - CompDataOff;
        if (CompLen > Skip) {
          UINT8 *Pe = NULL;
          UINTN  PeSz = 0;
          if (FindPe32InSectionStream (CompData + Skip, CompLen - Skip,
                                       &Pe, &PeSz)) {
            *PeOut = Pe;
            *PeSizeOut = PeSz;
            return TRUE;
          }
        }
      } else if (CompType == EFI_STANDARD_COMPRESSION) {
        UINT32 DestSize    = 0;
        UINT32 ScratchSize = 0;
        if (EFI_ERROR (LzmaUefiDecompressGetInfo (
                CompData, (UINT32)CompLen, &DestSize, &ScratchSize))) {
          DEBUG ((DEBUG_WARN, "AblUnwrap: LzmaGetInfo failed\n"));
          break;
        }
        UINT8 *Scratch = AllocatePool (ScratchSize);
        UINT8 *Dest    = AllocatePool (DestSize);
        if (Scratch == NULL || Dest == NULL) {
          if (Scratch != NULL) FreePool (Scratch);
          if (Dest    != NULL) FreePool (Dest);
          break;
        }
        EFI_STATUS St = LzmaUefiDecompress (
            CompData, (UINTN)CompLen, Dest, Scratch);
        FreePool (Scratch);
        if (EFI_ERROR (St)) {
          DEBUG ((DEBUG_WARN, "AblUnwrap: LzmaUefiDecompress failed: %r\n",
                  St));
          FreePool (Dest);
          break;
        }
        VERBOSE ("AblUnwrap: LZMA decompressed %u bytes\n",
                 DestSize);
        UINT8 *Pe = NULL;
        UINTN  PeSz = 0;
        if (FindPe32InSectionStream (Dest, DestSize, &Pe, &PeSz)) {
          FreePool (Dest);
          *PeOut = Pe;
          *PeSizeOut = PeSz;
          return TRUE;
        }
        if (ScanAndFindPe32 (Dest, DestSize, &Pe, &PeSz)) {
          FreePool (Dest);
          *PeOut = Pe;
          *PeSizeOut = PeSz;
          return TRUE;
        }
        FreePool (Dest);
      }
      break;
    }

    case EFI_SECTION_GUID_DEFINED:
    {
      if (SecDataSize < 20) {
        break;
      }
      EFI_GUID  Guid;
      UINT16    DataOffField;
      UINT8    *InnerData;
      UINTN     InnerSize;

      CopyMem (&Guid, SecData, sizeof (Guid));
      DataOffField = ReadLe16 (SecData + 16);

      if (DataOffField < SecHdrSize || DataOffField > SecSize) {
        DEBUG ((DEBUG_WARN,
                "AblUnwrap: invalid GUID_DEFINED data offset=%u sec=%u hdr=%u\n",
                (UINT32)DataOffField, (UINT32)SecSize,
                (UINT32)SecHdrSize));
        break;
      }

      InnerData = Buf + Offset + DataOffField;
      InnerSize = SecSize - DataOffField;

      VERBOSE ("AblUnwrap: GUID_DEFINED off=%u inner=%u\n",
               (UINT32)DataOffField, (UINT32)InnerSize);

      if (InnerSize > BufSize - (Offset + DataOffField)) {
        break;
      }

      if (CompareGuid (&Guid, &mLzmaGuid)) {
        UINT32 DestSize    = 0;
        UINT32 ScratchSize = 0;
        if (EFI_ERROR (LzmaUefiDecompressGetInfo (
                InnerData, (UINT32)InnerSize, &DestSize, &ScratchSize))) {
          DEBUG ((DEBUG_WARN, "AblUnwrap: GUID-LZMA GetInfo failed\n"));
          break;
        }
        UINT8 *Scratch = AllocatePool (ScratchSize);
        UINT8 *Dest    = AllocatePool (DestSize);
        if (Scratch == NULL || Dest == NULL) {
          if (Scratch != NULL) FreePool (Scratch);
          if (Dest    != NULL) FreePool (Dest);
          break;
        }
        EFI_STATUS St = LzmaUefiDecompress (
            InnerData, (UINTN)InnerSize, Dest, Scratch);
        FreePool (Scratch);
        if (EFI_ERROR (St)) {
          DEBUG ((DEBUG_WARN,
                  "AblUnwrap: GUID-LZMA Decompress failed: %r\n", St));
          FreePool (Dest);
          break;
        }
        GBL_INFO ("AblUnwrap: GUID-LZMA decompressed %u bytes\n", DestSize);
        UINT8 *Pe = NULL;
        UINTN  PeSz = 0;
        if (FindPe32InSectionStream (Dest, DestSize, &Pe, &PeSz)) {
          FreePool (Dest);
          *PeOut = Pe;
          *PeSizeOut = PeSz;
          return TRUE;
        }
        if (ScanAndFindPe32 (Dest, DestSize, &Pe, &PeSz)) {
          FreePool (Dest);
          *PeOut = Pe;
          *PeSizeOut = PeSz;
          return TRUE;
        }
        FreePool (Dest);
      } else {
        /* Unknown GUID — assume the inner data is a raw section stream
         * and just walk it. Aligns with gbl_root_canoe's fallback. */
        UINTN InnerStart     = Offset + DataOffField;
        UINTN InnerStartAlgn = (InnerStart + 3) & ~(UINTN)3;
        UINTN InnerEnd       = Offset + SecSize;
        if (InnerStartAlgn < InnerEnd) {
          UINT8 *Pe = NULL;
          UINTN  PeSz = 0;
          if (FindPe32InSectionStream (Buf + InnerStartAlgn,
                                       InnerEnd - InnerStartAlgn,
                                       &Pe, &PeSz)) {
            *PeOut = Pe;
            *PeSizeOut = PeSz;
            return TRUE;
          }
        }
      }
      break;
    }

    case EFI_SECTION_FIRMWARE_VOLUME_IMAGE:
    {
      VERBOSE ("AblUnwrap: FV_IMAGE section, scanning %u bytes\n",
               (UINT32)SecDataSize);
      UINT8 *Pe = NULL;
      UINTN  PeSz = 0;
      if (ScanAndFindPe32 (SecData, SecDataSize, &Pe, &PeSz)) {
        *PeOut = Pe;
        *PeSizeOut = PeSz;
        return TRUE;
      }
      break;
    }

    default:
      break;
    }

    Offset += SecSize;
  }

  return FALSE;
}

STATIC BOOLEAN
FindPe32InFv (
  IN  UINT8   *FvBuf,
  IN  UINTN    FvSize,
  OUT UINT8  **PeOut,
  OUT UINTN   *PeSizeOut
  )
{
  EFI_FIRMWARE_VOLUME_HEADER *FvH = (EFI_FIRMWARE_VOLUME_HEADER *)FvBuf;
  UINTN Offset;
  UINTN FvEnd;

  if (FvSize < sizeof (EFI_FIRMWARE_VOLUME_HEADER) ||
      FvH->Signature != EFI_FVH_SIGNATURE ||
      FvH->HeaderLength < sizeof (EFI_FIRMWARE_VOLUME_HEADER) ||
      FvH->HeaderLength > FvSize ||
      FvH->FvLength > (UINT64)FvSize) {
    DEBUG ((DEBUG_WARN, "AblUnwrap: invalid FV header\n"));
    return FALSE;
  }

  Offset = (FvH->HeaderLength + 7) & ~(UINTN)7;
  FvEnd  = (UINTN)FvH->FvLength;

  while (Offset + 24 <= FvEnd) {
    BOOLEAN AllFF;
    UINTN   FfsHdrSz;
    UINTN   FileSize;
    UINT8   FfsType;
    UINT8  *FileData;
    UINTN   FileDataSize;
    UINT8  *Pe = NULL;
    UINTN   PeSz = 0;
    UINTN   k;

    AllFF = TRUE;
    for (k = 0; k < 24; k++) {
      if (FvBuf[Offset + k] != 0xFF) {
        AllFF = FALSE;
        break;
      }
    }
    if (AllFF) {
      Offset += 8;
      continue;
    }

    FfsHdrSz = 0;
    if (!GetFfsSizeEx (FvBuf + Offset, FvEnd - Offset,
                       &FileSize, &FfsHdrSz)) {
      break;
    }
    FfsType  = FvBuf[Offset + 18];

    if (FfsType == EFI_FV_FILETYPE_FFS_PAD) {
      if (FileSize < FfsHdrSz) {
        break;
      }
      Offset = (Offset + FileSize + 7) & ~(UINTN)7;
      continue;
    }

    if (FileSize < FfsHdrSz || FileSize > FvEnd - Offset) {
      break;
    }

    FileData     = FvBuf + Offset + FfsHdrSz;
    FileDataSize = FileSize - FfsHdrSz;

    if (FindPe32InSectionStream (FileData, FileDataSize, &Pe, &PeSz)) {
      *PeOut     = Pe;
      *PeSizeOut = PeSz;
      return TRUE;
    }

    Offset = (Offset + FileSize + 7) & ~(UINTN)7;
  }

  return FALSE;
}

/* Fallback: scan for embedded "_FVH" anywhere in the buffer (some
 * Qualcomm packagings put the FV inside an ELF wrapper at non-zero
 * offset). */
STATIC BOOLEAN
ScanAndFindPe32 (
  IN  UINT8   *Buf,
  IN  UINTN    BufSize,
  OUT UINT8  **PeOut,
  OUT UINTN   *PeSizeOut
  )
{
  UINTN Off = 0;

  while (Off + 0x38 < BufSize) {
    UINTN i;
    UINTN Found = (UINTN)-1;
    UINT8 *FvStart;
    UINTN  FvRemain;
    EFI_FIRMWARE_VOLUME_HEADER H;
    UINT8 *Pe = NULL;
    UINTN  PeSz = 0;

    for (i = Off; i + 4 <= BufSize; i++) {
      if (Buf[i]   == '_' && Buf[i+1] == 'F' &&
          Buf[i+2] == 'V' && Buf[i+3] == 'H') {
        Found = i;
        break;
      }
    }
    if (Found == (UINTN)-1) {
      break;
    }

    if (Found < 0x28) {
      Off = Found + 4;
      continue;
    }

    FvStart  = Buf + Found - 0x28;
    FvRemain = BufSize - (UINTN)(FvStart - Buf);
    if (FvRemain < sizeof (EFI_FIRMWARE_VOLUME_HEADER)) {
      Off = Found + 4;
      continue;
    }
    CopyMem (&H, FvStart, sizeof (H));

    if (H.Signature == EFI_FVH_SIGNATURE &&
        H.HeaderLength >= 0x48    &&
        H.HeaderLength <= 0x200   &&
        H.FvLength > (UINT64)H.HeaderLength &&
        H.FvLength <= (UINT64)FvRemain) {
      if (FindPe32InFv (FvStart, (UINTN)H.FvLength, &Pe, &PeSz)) {
        *PeOut     = Pe;
        *PeSizeOut = PeSz;
        return TRUE;
      }
    }

    Off = Found + 4;
  }

  return FALSE;
}

EFI_STATUS
EFIAPI
AblUnwrap_LoadFromPartition (
  IN  CHAR16  *PartitionName,
  OUT VOID   **OutPe,
  OUT UINT32  *OutPeSize
  )
{
  EFI_STATUS  Status;
  VOID       *PartBuf  = NULL;
  UINTN       PartSize = 0;
  UINT8      *FvPtr;
  UINTN       FvSize   = 0;
  UINT8      *Pe       = NULL;
  UINTN       PeSize   = 0;

  if (PartitionName == NULL || OutPe == NULL || OutPeSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *OutPe     = NULL;
  *OutPeSize = 0;

  Status = ReadEntirePartition (PartitionName, &PartBuf, &PartSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* Try simple FV-at-offset-N search first (covers canoe). */
  FvPtr = FindFirmwareVolume ((UINT8 *)PartBuf, PartSize, &FvSize);
  if (FvPtr != NULL) {
    GBL_INFO ("AblUnwrap: FV @ offset 0x%lx size 0x%lx\n",
              (UINT64)((UINT8 *)FvPtr - (UINT8 *)PartBuf), (UINT64)FvSize);
    if (FindPe32InFv (FvPtr, FvSize, &Pe, &PeSize)) {
      goto Found;
    }
  }

  /* Fallback: scan for embedded "_FVH" anywhere in the partition. */
  GBL_INFO ("AblUnwrap: simple FV scan failed, trying _FVH scan\n");
  if (ScanAndFindPe32 ((UINT8 *)PartBuf, PartSize, &Pe, &PeSize)) {
    goto Found;
  }

  DEBUG ((DEBUG_ERROR, "AblUnwrap: PE32/TE not found in %s\n", PartitionName));
  FreePool (PartBuf);
  return EFI_NOT_FOUND;

Found:
  GBL_INFO ("AblUnwrap: PE/TE size 0x%lx\n", (UINT64)PeSize);
  FreePool (PartBuf);
  *OutPe     = Pe;
  *OutPeSize = (UINT32)PeSize;
  return EFI_SUCCESS;
}
