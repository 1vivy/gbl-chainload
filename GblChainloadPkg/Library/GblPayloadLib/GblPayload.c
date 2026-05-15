/* GblChainloadPkg/Library/GblPayloadLib/GblPayload.c
   Top-level public-API implementation. Glues LocateOverlayBytes +
   PayloadParse together. */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/GblLog.h>
#include <Library/GblPayloadLib.h>
#include "Internal/PayloadParse.h"
#include "../../../tools/shared/gblp1.h"

EFI_STATUS LocateOverlayBytes(OUT VOID **Bytes, OUT UINTN *Size);

/* Find "GBLP1\0\0\0" in the bytes, starting from offset 0. */
static EFI_STATUS
ScanForGblp1 (CONST UINT8 *Buf, UINTN Size, OUT UINTN *Off) {
  for (UINTN I = 0; I + GBLP1_MAGIC_SIZE <= Size; I++) {
    if (Buf[I] == 'G' &&
        CompareMem (Buf + I, GBLP1_MAGIC, GBLP1_MAGIC_SIZE) == 0) {
      *Off = I;
      return EFI_SUCCESS;
    }
  }
  return EFI_NOT_FOUND;
}

EFI_STATUS EFIAPI
GblPayload_LoadCachedAbl (IN EFI_HANDLE ImageHandle,
                          OUT VOID **Pe, OUT UINT32 *PeSize) {
  VOID *Bytes = NULL; UINTN Size = 0;
  EFI_STATUS Status = LocateOverlayBytes(&Bytes, &Size);
  if (EFI_ERROR(Status)) {
    GBL_INFO("gbl-payload: cannot locate overlay bytes (%r)\n", Status);
    return Status;
  }

  UINTN Off = 0;
  Status = ScanForGblp1((UINT8 *)Bytes, Size, &Off);
  if (EFI_ERROR(Status)) {
    GBL_INFO("gbl-payload: bad magic (no GBLP1 in source)\n");
    return EFI_LOAD_ERROR;
  }

  CONST UINT8 *PayloadBytes = (CONST UINT8 *)Bytes + Off;
  UINTN PayloadSize = Size - Off;

  CONST UINT8 *CachedPe = NULL; size_t CachedSize = 0;
  enum gbl_payload_status PS =
      gbl_payload_find_cached_abl(PayloadBytes, PayloadSize,
                                  &CachedPe, &CachedSize);
  if (PS != GBL_PAYLOAD_OK) {
    GBL_INFO("gbl-payload: parse status=%d\n", (int)PS);
    return EFI_LOAD_ERROR;
  }

  VOID *Copy = AllocatePool(CachedSize);
  if (!Copy) return EFI_OUT_OF_RESOURCES;
  CopyMem(Copy, CachedPe, CachedSize);

  *Pe = Copy;
  *PeSize = (UINT32)CachedSize;
  return EFI_SUCCESS;
}

VOID EFIAPI
GblPayload_LogProvenance (IN EFI_HANDLE ImageHandle) {
  /* For v1, log only the source. Walking source_meta is optional and
     can land in a follow-up if we want richer provenance. */
  GBL_INFO("gbl-payload: LogProvenance hook (source-meta walk: not yet wired)\n");
}
