#ifndef GBL_PAYLOAD_PARSE_H_
#define GBL_PAYLOAD_PARSE_H_

#ifdef GBL_HOST_BUILD
# include <stdint.h>
# include <stddef.h>
#else
# include <Uefi.h>
  typedef UINT8  uint8_t;
  typedef UINT16 uint16_t;
  typedef UINT32 uint32_t;
  typedef UINTN  size_t;
#endif

enum gbl_payload_status {
    GBL_PAYLOAD_OK = 0,
    GBL_PAYLOAD_TOO_SMALL,
    GBL_PAYLOAD_BAD_MAGIC,
    GBL_PAYLOAD_BAD_VERSION,
    GBL_PAYLOAD_BAD_HEADER_SIZE,
    GBL_PAYLOAD_BAD_FLAGS,
    GBL_PAYLOAD_BAD_TOTAL_SIZE,
    GBL_PAYLOAD_BAD_ENTRY_COUNT,
    GBL_PAYLOAD_HEADER_CRC_MISMATCH,
    GBL_PAYLOAD_FOOTER_MISMATCH,
    GBL_PAYLOAD_ENTRY_BAD_TYPE,
    GBL_PAYLOAD_ENTRY_BAD_FLAGS,
    GBL_PAYLOAD_ENTRY_BAD_RESERVED,
    GBL_PAYLOAD_ENTRY_BAD_OFFSET,
    GBL_PAYLOAD_ENTRY_BAD_SIZE,
    GBL_PAYLOAD_ENTRY_SHA_MISMATCH,
    GBL_PAYLOAD_NO_CACHED_ABL,
    GBL_PAYLOAD_PE_INSANE
};

/* Validates only the GBLP1 header + footer layout. Does NOT walk
   entries or hash payloads. Used as a fast pre-check. */
enum gbl_payload_status
gbl_payload_validate_header(const uint8_t *bytes, size_t size);

#endif
