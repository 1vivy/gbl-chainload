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

#endif /* GBL_STAGED_BUFFER_H_ */
