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

#endif
