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
#include "../../../crates/gblp1/include/gblp1_ffi.h"
#include "../../../crates/mode2-profile-core/include/mode2_profile_ffi.h"

EFI_STATUS LocateOverlayBytes(OUT VOID **Bytes, OUT UINTN *Size);

/* Single firmware-wide engine manifest. Populated by GblPayload_LoadManifest
   from BootFlow once per boot; consumed by ProtocolHookLib hook bodies and
   the BootFlow mode-2 gate. Defaults to all-zero (effective mode-0). */
struct GblManifest gManifest = {0};

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

EFI_STATUS EFIAPI
GblPayload_LoadManifest (IN  EFI_HANDLE          ImageHandle,
                         OUT struct GblManifest *Manifest) {
  VOID *Bytes = NULL; UINTN Size = 0;
  if (Manifest == NULL) return EFI_INVALID_PARAMETER;

  /* Default: all-FALSE (safe mode-0). Set first so every error path
     leaves the caller with a defined, all-zero manifest. */
  Manifest->WantFakelockHook = FALSE;
  Manifest->WantProfileSpoof = FALSE;

  EFI_STATUS Status = LocateOverlayBytes(&Bytes, &Size);
  if (EFI_ERROR(Status)) {
    GBL_INFO("gbl-payload: manifest — no overlay bytes (%r)\n", Status);
    return EFI_NOT_FOUND;
  }

  /* Scan for the GBLP1 magic, tolerating stray copies, then locate the
     manifest entry within the first fully-valid container. Mirrors the
     same loop used by GblPayload_LoadMode2Profile. */
  CONST UINT8 *B = (CONST UINT8 *)Bytes;
  enum gbl_payload_status PS = GBL_PAYLOAD_BAD_MAGIC;
  struct gbl_manifest Wire = {0};
  int Present = 0;
  BOOLEAN Located = FALSE;
  for (UINTN i = 0; i + GBLP1_MAGIC_SIZE <= Size; i++) {
    if (CompareMem(B + i, GBLP1_MAGIC, GBLP1_MAGIC_SIZE) != 0) continue;
    PS = gbl_payload_find_manifest(B + i, Size - i, &Wire, &Present);
    if (PS == GBL_PAYLOAD_OK) { Located = TRUE; break; }
  }

  if (!Located) {
    if (PS == GBL_PAYLOAD_BAD_MAGIC) {
      GBL_INFO("gbl-payload: manifest — no GBLP1 magic in overlay\n");
      return EFI_NOT_FOUND;
    }
    GBL_INFO("gbl-payload: manifest — container invalid (status=%d)\n",
             (int)PS);
    return EFI_LOAD_ERROR;
  }

  if (!Present) {
    /* Container valid, but no 0x0020 entry. Forward-compat: old GBLP1
       overlays predate the manifest type; treat absence as the safe
       all-zero default (mode-0 / pure observation). */
    GBL_INFO("gbl-payload: manifest — absent; defaulting all caps to 0\n");
    return EFI_SUCCESS;
  }

  Manifest->WantFakelockHook =
      (Wire.cap_bits & GBLP1_MANIFEST_BIT_FAKELOCK_HOOK) ? TRUE : FALSE;
  Manifest->WantProfileSpoof =
      (Wire.cap_bits & GBLP1_MANIFEST_BIT_PROFILE_SPOOF) ? TRUE : FALSE;

  GBL_INFO("gbl-payload: manifest — loaded (caps=0x%04x fakelock=%u spoof=%u)\n",
           (UINT32)Wire.cap_bits,
           (UINT32)Manifest->WantFakelockHook,
           (UINT32)Manifest->WantProfileSpoof);
  return EFI_SUCCESS;
}
