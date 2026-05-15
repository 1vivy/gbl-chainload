#ifndef GBL_CACHED_ABL_LIB_H_
#define GBL_CACHED_ABL_LIB_H_

#include <Uefi.h>

BOOLEAN
EFIAPI
CachedAbl_IsPresent (VOID);

EFI_STATUS
EFIAPI
CachedAbl_CopyPe (
  OUT VOID   **Pe,
  OUT UINT32  *PeSize
  );

VOID
EFIAPI
CachedAbl_LogMetadata (VOID);

#endif /* GBL_CACHED_ABL_LIB_H_ */
