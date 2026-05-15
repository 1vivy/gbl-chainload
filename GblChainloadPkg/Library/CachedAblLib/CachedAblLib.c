/** @file CachedAblLib.c — optional build-time cached ABL payload. */

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/GblLog.h>
#include <Library/CachedAblLib.h>

#ifndef GBL_HAS_CACHED_ABL
# define GBL_HAS_CACHED_ABL 0
#endif

#if GBL_HAS_CACHED_ABL
# include "CachedAblBlob.generated.h"
#else
STATIC CONST UINT8  gCachedAblPe[] = { 0 };
STATIC CONST UINT32 gCachedAblPeSize = 0;
STATIC CONST CHAR8  gCachedAblSourceSha256[] = "none";
STATIC CONST CHAR8  gCachedAblExtractedPeSha256[] = "none";
STATIC CONST CHAR8  gCachedAblPatchedPeSha256[] = "none";
STATIC CONST CHAR8  gCachedAblBuildMode[] = "none";
#endif

BOOLEAN
EFIAPI
CachedAbl_IsPresent (VOID)
{
  return (GBL_HAS_CACHED_ABL && gCachedAblPeSize > 0);
}

EFI_STATUS
EFIAPI
CachedAbl_CopyPe (
  OUT VOID   **Pe,
  OUT UINT32  *PeSize
  )
{
  VOID *Copy;

  if (Pe == NULL || PeSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *Pe = NULL;
  *PeSize = 0;

  if (!CachedAbl_IsPresent ()) {
    return EFI_NOT_FOUND;
  }

  Copy = AllocatePool (gCachedAblPeSize);
  if (Copy == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (Copy, gCachedAblPe, gCachedAblPeSize);
  *Pe = Copy;
  *PeSize = gCachedAblPeSize;
  return EFI_SUCCESS;
}

VOID
EFIAPI
CachedAbl_LogMetadata (VOID)
{
  if (!CachedAbl_IsPresent ()) {
    GBL_INFO ("CachedAbl: absent\n");
    return;
  }

  GBL_INFO ("CachedAbl: present size=%u mode=%a source_sha256=%a extracted_pe_sha256=%a patched_pe_sha256=%a\n",
            gCachedAblPeSize,
            gCachedAblBuildMode,
            gCachedAblSourceSha256,
            gCachedAblExtractedPeSha256,
            gCachedAblPatchedPeSha256);
}
