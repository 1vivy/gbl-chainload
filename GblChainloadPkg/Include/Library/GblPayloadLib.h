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

#include "../../../tools/shared/gbl_mode2_profile.h"

/* Locate the GBLP1 overlay, find the mode2_profile (0x0010) entry, and
   parse it. Returns:
     EFI_SUCCESS    — *Profile filled with a validated profile
     EFI_NOT_FOUND  — no overlay, or no 0x0010 entry in the overlay
     EFI_LOAD_ERROR — overlay/entry present but failed validation */
EFI_STATUS EFIAPI
GblPayload_LoadMode2Profile (IN  EFI_HANDLE                ImageHandle,
                             OUT struct gbl_mode2_profile *Profile);

#endif
