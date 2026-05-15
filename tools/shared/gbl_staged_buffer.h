/* tools/shared/gbl_staged_buffer.h
   Configuration table installed by FastbootLib's `oem boot-efi` handler
   so an overlay-aware EFI (e.g., gbl-chainload's GblPayloadLib) can find
   the staged buffer it was loaded from.

   GUID generated 2026-05-15 — keep stable; changing it breaks the test
   path's contract with FastbootLib. */

#ifndef GBL_STAGED_BUFFER_H_
#define GBL_STAGED_BUFFER_H_

/* Magic value the table must carry: SIGNATURE_32('G','B','L','S') */
#define GBL_STAGED_BUFFER_MAGIC   0x534C4247u  /* 'G''B''L''S' little-endian */
#define GBL_STAGED_BUFFER_VERSION 1u

/* Define the GUID once. Both the producer (FastbootCmds.c) and the
   consumer (LocateOverlay.c) reference it via this header. The literal
   shape works under both EDK2 (uses { ... } init) and host (typedef'd
   for tests if any).

   UUID: bb230682-6c4c-40c9-9b8c-73b541ce9ba4 */
#define GBL_STAGED_BUFFER_GUID \
    { 0xbb230682, 0x6c4c, 0x40c9, \
      { 0x9b, 0x8c, 0x73, 0xb5, 0x41, 0xce, 0x9b, 0xa4 } }

/* Shared struct used by the producer (FastbootCmds.c) and the consumer
   (LocateOverlay.c).  Both include <Uefi.h> before this header, so
   EFI_PHYSICAL_ADDRESS and UINTN are available.  This header is only
   ever included from EDK2 translation units — no host-build guard needed. */
typedef struct {
  UINT32                Magic;    /* must equal GBL_STAGED_BUFFER_MAGIC   */
  UINT32                Version;  /* must equal GBL_STAGED_BUFFER_VERSION */
  EFI_PHYSICAL_ADDRESS  Base;     /* physical address of the staged buffer */
  UINTN                 Size;     /* size of the staged buffer in bytes    */
} GBL_STAGED_BUFFER_TABLE;

#endif /* GBL_STAGED_BUFFER_H_ */
