/* GblChainloadPkg/Library/GblPayloadLib/LocateOverlay.c
   Decision: prefer staged-buffer config table (test path) over EFISP
   raw read (production path). */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/GblLog.h>

#include "../../../tools/shared/gbl_staged_buffer.h"

EFI_GUID gGblStagedBufferGuid = GBL_STAGED_BUFFER_GUID;

typedef struct {
    UINT32                Magic;
    UINT32                Version;
    EFI_PHYSICAL_ADDRESS  Base;
    UINTN                 Size;
} GBL_STAGED_BUFFER_TABLE;

EFI_STATUS ReadEfispRawBytes(VOID **OutBytes, UINTN *OutSize); /* in EfispBlockIo.c — lands in T2.5 */

EFI_STATUS
LocateOverlayBytes (OUT VOID **Bytes, OUT UINTN *Size)
{
  for (UINTN I = 0; I < gST->NumberOfTableEntries; I++) {
    if (CompareGuid(&gST->ConfigurationTable[I].VendorGuid,
                    &gGblStagedBufferGuid)) {
      GBL_STAGED_BUFFER_TABLE *T = gST->ConfigurationTable[I].VendorTable;
      if (T && T->Magic == GBL_STAGED_BUFFER_MAGIC && T->Version == GBL_STAGED_BUFFER_VERSION) {
        *Bytes = (VOID *)(UINTN)T->Base;
        *Size  = T->Size;
        GBL_INFO("gbl-payload: source=staged-buffer base=0x%lx size=%u\n",
                 (UINT64)T->Base, (UINT32)T->Size);
        return EFI_SUCCESS;
      }
    }
  }
  GBL_INFO("gbl-payload: source=efisp-blockio (no staged-buffer table)\n");
  return ReadEfispRawBytes(Bytes, Size);
}
