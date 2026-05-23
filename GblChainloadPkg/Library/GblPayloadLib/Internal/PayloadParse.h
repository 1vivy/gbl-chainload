#ifndef GBL_PAYLOAD_PARSE_H_
#define GBL_PAYLOAD_PARSE_H_

#ifdef GBL_HOST_BUILD
# include <stdint.h>
# include <stddef.h>
#else
# include <Uefi.h>
# ifndef GBL_COMPAT_TYPES_DEFINED
#  define GBL_COMPAT_TYPES_DEFINED
   typedef UINT8  uint8_t;
   typedef UINT16 uint16_t;
   typedef UINT32 uint32_t;
   typedef INT32  int32_t;
# endif
# ifndef _SIZE_T
#  define _SIZE_T
   typedef __SIZE_TYPE__ size_t;
# endif
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
    GBL_PAYLOAD_NO_MODE2_PROFILE,
    /* Reserved; not currently returned. gbl_payload_find_manifest()
       signals absence via GBL_PAYLOAD_OK + *out_present=0. */
    GBL_PAYLOAD_NO_MANIFEST,
    GBL_PAYLOAD_BAD_MANIFEST_MAGIC,
    GBL_PAYLOAD_BAD_MANIFEST_SCHEMA,
    GBL_PAYLOAD_BAD_MANIFEST_RESERVED,
    GBL_PAYLOAD_BAD_MANIFEST_SIZE
};

/* Validates only the GBLP1 header + footer layout. Does NOT walk
   entries or hash payloads. Used as a fast pre-check. */
enum gbl_payload_status
gbl_payload_validate_header(const uint8_t *bytes, size_t size);

/* Validates header + walks every entry (type/flags/reserved/offset/size checks
   + per-entry SHA-256 verify) and locates the unique GBLP1_TYPE_CACHED_ABL
   entry.  No PE structural check is done here — gBS->LoadImage is the runtime
   authority; PE sanity is a host pre-flight in tools/gbl-pack.  On
   GBL_PAYLOAD_OK, *out_pe points into `bytes` and *out_pe_size is its
   byte length. */
enum gbl_payload_status
gbl_payload_find_cached_abl(const uint8_t *bytes, size_t size,
                            const uint8_t **out_pe, size_t *out_pe_size);

/* Validates header + walks every entry exactly as
   gbl_payload_find_cached_abl, then locates the unique
   GBLP1_TYPE_MODE2_PROFILE (0x0010) entry. On GBL_PAYLOAD_OK,
   *out_profile points into `bytes` and *out_size is its length.
   Returns GBL_PAYLOAD_NO_MODE2_PROFILE if no 0x0010 entry exists. */
enum gbl_payload_status
gbl_payload_find_mode2_profile(const uint8_t *bytes, size_t size,
                               const uint8_t **out_profile,
                               size_t *out_size);

/* Scan bytes[0..size) for a GBLP1 container: at each occurrence of the
   8-byte magic, attempt gbl_payload_find_cached_abl on the sub-buffer;
   return the result of the FIRST occurrence that fully validates.  This
   tolerates stray copies of the magic (e.g. the GBLP1_MAGIC string
   literal embedded in a preceding PE's .rodata).
   Returns the last non-OK status if the magic was seen but none validated,
   or GBL_PAYLOAD_BAD_MAGIC if the magic was never found. */
enum gbl_payload_status
gbl_payload_scan_cached_abl(const uint8_t *bytes, size_t size,
                            const uint8_t **out_pe, size_t *out_pe_size);

/* Engine capability manifest. cap_bits is the raw bit field from the
   wire, validated against GBLP1_MANIFEST_BITS_RESERVED_MASK but otherwise
   passed through — callers should compare against the
   GBLP1_MANIFEST_BIT_* constants. */
struct gbl_manifest { uint16_t cap_bits; };

/* Locate + validate the unique GBLP1_TYPE_MANIFEST entry. On
   GBL_PAYLOAD_OK with *out_present == 1: *out is filled. On
   *out_present == 0: no manifest entry exists in the container
   (NOT an error; caller treats as all-zero capabilities). On any
   other return: parse or validation failed; *out is undefined. */
enum gbl_payload_status
gbl_payload_find_manifest(const uint8_t *bytes, size_t size,
                          struct gbl_manifest *out, int *out_present);

#endif
