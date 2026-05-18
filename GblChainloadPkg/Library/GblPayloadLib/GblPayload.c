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
#include "Internal/Mode2Profile.h"
#include "../../../tools/shared/gblp1.h"

EFI_STATUS LocateOverlayBytes(OUT VOID **Bytes, OUT UINTN *Size);

EFI_STATUS EFIAPI
GblPayload_LoadCachedAbl (IN EFI_HANDLE ImageHandle,
                          OUT VOID **Pe, OUT UINT32 *PeSize) {
  VOID *Bytes = NULL; UINTN Size = 0;
  EFI_STATUS Status = LocateOverlayBytes(&Bytes, &Size);
  if (EFI_ERROR(Status)) {
    GBL_INFO("gbl-payload: cannot locate overlay bytes (%r)\n", Status);
    return Status;
  }

  CONST UINT8 *CachedPe = NULL; size_t CachedSize = 0;
  enum gbl_payload_status PS =
      gbl_payload_scan_cached_abl((CONST UINT8 *)Bytes, Size,
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

EFI_STATUS EFIAPI
GblPayload_LoadMode2Profile (IN  EFI_HANDLE                ImageHandle,
                             OUT struct gbl_mode2_profile *Profile) {
  VOID *Bytes = NULL; UINTN Size = 0;
  if (Profile == NULL) return EFI_INVALID_PARAMETER;

  EFI_STATUS Status = LocateOverlayBytes(&Bytes, &Size);
  if (EFI_ERROR(Status)) {
    GBL_INFO("gbl-payload: mode2 — no overlay bytes (%r)\n", Status);
    return EFI_NOT_FOUND;
  }

  /* Scan for the GBLP1 magic, tolerating stray copies, then locate the
     0x0010 entry within the first fully-valid container. */
  CONST UINT8 *B = (CONST UINT8 *)Bytes;
  enum gbl_payload_status PS = GBL_PAYLOAD_BAD_MAGIC;
  CONST UINT8 *ProfBytes = NULL; size_t ProfSize = 0;
  for (UINTN i = 0; i + GBLP1_MAGIC_SIZE <= Size; i++) {
    if (CompareMem(B + i, GBLP1_MAGIC, GBLP1_MAGIC_SIZE) != 0) continue;
    PS = gbl_payload_find_mode2_profile(B + i, Size - i,
                                        &ProfBytes, &ProfSize);
    if (PS == GBL_PAYLOAD_OK || PS == GBL_PAYLOAD_NO_MODE2_PROFILE) break;
  }

  if (PS == GBL_PAYLOAD_BAD_MAGIC) {
    GBL_INFO("gbl-payload: mode2 — no GBLP1 magic in overlay\n");
    return EFI_NOT_FOUND;
  }
  if (PS == GBL_PAYLOAD_NO_MODE2_PROFILE) {
    GBL_INFO("gbl-payload: mode2 — no 0x0010 entry in container\n");
    return EFI_NOT_FOUND;
  }
  if (PS != GBL_PAYLOAD_OK) {
    GBL_INFO("gbl-payload: mode2 — container invalid (status=%d)\n", (int)PS);
    return EFI_LOAD_ERROR;
  }

  enum gbl_m2p_status MS =
      gbl_mode2_profile_parse(ProfBytes, ProfSize, Profile);
  if (MS != GBL_M2P_OK) {
    GBL_INFO("gbl-payload: mode2 — profile invalid (status=%d)\n", (int)MS);
    return EFI_LOAD_ERROR;
  }
  GBL_INFO("gbl-payload: mode2 — profile loaded (ver=%u color=%u)\n",
           Profile->version, Profile->color);
  return EFI_SUCCESS;
}
