/* GblChainloadPkg/Include/Library/GblPayloadLib.h — EDK2 public API. */
#ifndef GBL_PAYLOAD_LIB_H_
#define GBL_PAYLOAD_LIB_H_

#include <Uefi.h>

EFI_STATUS EFIAPI
GblPayload_LoadCachedAbl (IN  EFI_HANDLE  ImageHandle,
                          OUT VOID      **Pe,
                          OUT UINT32     *PeSize);

VOID EFIAPI
GblPayload_LogProvenance (IN EFI_HANDLE  ImageHandle);

#include "../../../crates/mode2-profile-core/include/mode2_profile_ffi.h"

/* Locate the GBLP1 overlay, find the mode2_profile (0x0010) entry, and
   parse it. Returns:
     EFI_SUCCESS    — *Profile filled with a validated profile
     EFI_NOT_FOUND  — no overlay, or no 0x0010 entry in the overlay
     EFI_LOAD_ERROR — overlay/entry present but failed validation */
EFI_STATUS EFIAPI
GblPayload_LoadMode2Profile (IN  EFI_HANDLE                ImageHandle,
                             OUT struct gbl_mode2_profile *Profile);

/* Engine capability manifest, firmware-facing form. The wire-level
   cap_bits field is translated into named booleans here so call sites
   read as `if (gManifest.WantFakelockHook)` instead of bit math. */
struct GblManifest {
  BOOLEAN WantFakelockHook;
  BOOLEAN WantProfileSpoof;
};

/* Single firmware-wide manifest instance. Owned by GblPayloadLib;
   populated by GblPayload_LoadManifest() once per boot in BootFlow.
   Defaults to all-FALSE (effective mode-0 / pure observation). */
extern struct GblManifest gManifest;

/* Locate the GBLP1 overlay, find the GBLP1_TYPE_MANIFEST (0x0020) entry,
   and translate its cap_bits into *Manifest. Returns:
     EFI_SUCCESS    — present and valid, OR absent (then *Manifest is
                      all-FALSE; absence is the safe mode-0 default)
     EFI_NOT_FOUND  — no overlay at all (no GBLP1 magic anywhere)
     EFI_LOAD_ERROR — overlay/entry present but failed validation
   On EFI_NOT_FOUND, *Manifest is zeroed so callers can treat the result
   as "all capabilities cleared." */
EFI_STATUS EFIAPI
GblPayload_LoadManifest (IN  EFI_HANDLE          ImageHandle,
                         OUT struct GblManifest *Manifest);

#endif
