/** @file CachedAblLib.c — optional build-time cached ABL payload. */

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/GblLog.h>
#include <Library/CachedAblLib.h>
#include <Library/CachedAblLayout.h>

#ifndef GBL_HAS_CACHED_ABL
# define GBL_HAS_CACHED_ABL 0
#endif

#if GBL_HAS_CACHED_ABL
# include "CachedAblBlob.generated.h"
#else
STATIC CONST UINT8  gCachedAblContainer[] = { 0 };
STATIC CONST UINT32 gCachedAblContainerSize = 0;
#endif

STATIC UINT32
ReadLe32 (
  IN CONST UINT8 *P
  )
{
  return ((UINT32)P[0]) |
         ((UINT32)P[1] << 8) |
         ((UINT32)P[2] << 16) |
         ((UINT32)P[3] << 24);
}

STATIC VOID
SafeAsciiCopy64 (
  OUT CHAR8 *Dst,
  IN CONST CHAR8 *Src
  )
{
  UINTN I;
  for (I = 0; I < GBL_CACHED_ABL_SHA_ASCII_SIZE; ++I) {
    Dst[I] = Src[I];
  }
  Dst[GBL_CACHED_ABL_SHA_ASCII_SIZE] = '\0';
}

STATIC BOOLEAN
ContainerValidAt (
  IN CONST UINT8 *Container,
  IN UINT32 ContainerSize,
  OUT UINT32 *PayloadSize OPTIONAL,
  OUT UINT32 *PayloadOffset OPTIONAL,
  OUT UINT32 *Mode OPTIONAL,
  OUT UINT32 *SourceSize OPTIONAL,
  OUT CONST CHAR8 **SourceSha OPTIONAL,
  OUT CONST CHAR8 **ExtractedSha OPTIONAL,
  OUT CONST CHAR8 **PatchedSha OPTIONAL
  )
{
  UINT32 Version;
  UINT32 Present;
  UINT32 Capacity;
  UINT32 Size;
  UINT32 EndMagic;

  if (Container == NULL || ContainerSize < GBL_CACHED_ABL_PAYLOAD_OFFSET + GBL_CACHED_ABL_MAGIC_SIZE) {
    return FALSE;
  }
  if (CompareMem (Container + GBL_CACHED_ABL_OFF_MAGIC,
                  GBL_CACHED_ABL_MAGIC,
                  GBL_CACHED_ABL_MAGIC_SIZE) != 0) {
    return FALSE;
  }

  Version = ReadLe32 (Container + GBL_CACHED_ABL_OFF_VERSION);
  Present = ReadLe32 (Container + GBL_CACHED_ABL_OFF_PRESENT);
  Capacity = ReadLe32 (Container + GBL_CACHED_ABL_OFF_CAPACITY);
  Size = ReadLe32 (Container + GBL_CACHED_ABL_OFF_PAYLOAD_SIZE);
  EndMagic = GBL_CACHED_ABL_PAYLOAD_OFFSET + Capacity;

  if (Version != GBL_CACHED_ABL_VERSION || Present != 1 || Capacity == 0 || Size == 0 || Size > Capacity) {
    return FALSE;
  }
  if (EndMagic + GBL_CACHED_ABL_MAGIC_SIZE > ContainerSize) {
    return FALSE;
  }
  if (CompareMem (Container + EndMagic,
                  GBL_CACHED_ABL_MAGIC,
                  GBL_CACHED_ABL_MAGIC_SIZE) != 0) {
    return FALSE;
  }

  if (PayloadSize != NULL) {
    *PayloadSize = Size;
  }
  if (PayloadOffset != NULL) {
    *PayloadOffset = GBL_CACHED_ABL_PAYLOAD_OFFSET;
  }
  if (Mode != NULL) {
    *Mode = ReadLe32 (Container + GBL_CACHED_ABL_OFF_MODE);
  }
  if (SourceSize != NULL) {
    *SourceSize = ReadLe32 (Container + GBL_CACHED_ABL_OFF_SOURCE_SIZE);
  }
  if (SourceSha != NULL) {
    *SourceSha = (CONST CHAR8 *)(Container + GBL_CACHED_ABL_OFF_SOURCE_SHA);
  }
  if (ExtractedSha != NULL) {
    *ExtractedSha = (CONST CHAR8 *)(Container + GBL_CACHED_ABL_OFF_EXTRACTED_SHA);
  }
  if (PatchedSha != NULL) {
    *PatchedSha = (CONST CHAR8 *)(Container + GBL_CACHED_ABL_OFF_PATCHED_SHA);
  }
  return TRUE;
}

STATIC BOOLEAN
CachedContainerValid (
  OUT UINT32 *PayloadSize OPTIONAL,
  OUT UINT32 *PayloadOffset OPTIONAL,
  OUT UINT32 *Mode OPTIONAL,
  OUT UINT32 *SourceSize OPTIONAL,
  OUT CONST CHAR8 **SourceSha OPTIONAL,
  OUT CONST CHAR8 **ExtractedSha OPTIONAL,
  OUT CONST CHAR8 **PatchedSha OPTIONAL
  )
{
  if (!GBL_HAS_CACHED_ABL) {
    return FALSE;
  }
  return ContainerValidAt (gCachedAblContainer, gCachedAblContainerSize,
                           PayloadSize, PayloadOffset, Mode, SourceSize,
                           SourceSha, ExtractedSha, PatchedSha);
}

BOOLEAN
EFIAPI
CachedAbl_IsPresent (VOID)
{
  return CachedContainerValid (NULL, NULL, NULL, NULL, NULL, NULL, NULL);
}

EFI_STATUS
EFIAPI
CachedAbl_CopyPe (
  OUT VOID   **Pe,
  OUT UINT32  *PeSize
  )
{
  VOID *Copy;
  UINT32 Size;
  UINT32 Offset;

  if (Pe == NULL || PeSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *Pe = NULL;
  *PeSize = 0;

  if (!CachedContainerValid (&Size, &Offset, NULL, NULL, NULL, NULL, NULL)) {
    return EFI_NOT_FOUND;
  }

  Copy = AllocatePool (Size);
  if (Copy == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (Copy, gCachedAblContainer + Offset, Size);
  *Pe = Copy;
  *PeSize = Size;
  return EFI_SUCCESS;
}

VOID
EFIAPI
CachedAbl_LogMetadata (VOID)
{
  UINT32 Size;
  UINT32 Mode;
  UINT32 SourceSize;
  CONST CHAR8 *SourceSha;
  CONST CHAR8 *ExtractedSha;
  CONST CHAR8 *PatchedSha;
  CHAR8 SourceBuf[GBL_CACHED_ABL_SHA_ASCII_SIZE + 1];
  CHAR8 ExtractedBuf[GBL_CACHED_ABL_SHA_ASCII_SIZE + 1];
  CHAR8 PatchedBuf[GBL_CACHED_ABL_SHA_ASCII_SIZE + 1];

  if (!CachedContainerValid (&Size, NULL, &Mode, &SourceSize, &SourceSha, &ExtractedSha, &PatchedSha)) {
    GBL_INFO ("CachedAbl: absent\n");
    return;
  }

  SafeAsciiCopy64 (SourceBuf, SourceSha);
  SafeAsciiCopy64 (ExtractedBuf, ExtractedSha);
  SafeAsciiCopy64 (PatchedBuf, PatchedSha);

  GBL_INFO ("CachedAbl: present size=%u mode=%u source_size=%u source_sha256=%a extracted_pe_sha256=%a patched_pe_sha256=%a\n",
            Size,
            Mode,
            SourceSize,
            SourceBuf,
            ExtractedBuf,
            PatchedBuf);
}
