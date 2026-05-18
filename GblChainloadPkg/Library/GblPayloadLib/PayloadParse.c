/* GblChainloadPkg/Library/GblPayloadLib/PayloadParse.c — pure-logic
   parser. The EDK2 IO wrapper (LocateOverlay.c, EfispBlockIo.c) calls
   into this with a ready-to-validate byte buffer. */
#include "Internal/PayloadParse.h"
#ifdef GBL_HOST_BUILD
# include <string.h>
#else
# include <Library/BaseMemoryLib.h>
  /* Map C99 memcmp to EDK2 CompareMem (same semantics for equality test). */
# define memcmp(a,b,n) CompareMem((a),(b),(n))
#endif
#include "Internal/Crc32.h"
#include "../../../tools/shared/gblp1.h"

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

enum gbl_payload_status
gbl_payload_validate_header(const uint8_t *b, size_t n) {
    if (n < (size_t)(GBLP1_HEADER_SIZE + GBLP1_ENTRY_SIZE + GBLP1_FOOTER_SIZE))
        return GBL_PAYLOAD_TOO_SMALL;
    if (memcmp(b, GBLP1_MAGIC, GBLP1_MAGIC_SIZE) != 0)
        return GBL_PAYLOAD_BAD_MAGIC;
    if (le16(b + 8) != GBLP1_VERSION)        return GBL_PAYLOAD_BAD_VERSION;
    if (le16(b + 10) != GBLP1_HEADER_SIZE)   return GBL_PAYLOAD_BAD_HEADER_SIZE;
    uint32_t flags = le32(b + 12);
    if (!(flags & GBLP1_FLAGS_LE) || (flags & ~GBLP1_FLAGS_LE))
        return GBL_PAYLOAD_BAD_FLAGS;
    uint32_t total = le32(b + 16);
    if (total > GBLP1_TOTAL_SIZE_CAP || (size_t)total > n)
        return GBL_PAYLOAD_BAD_TOTAL_SIZE;
    uint32_t ec = le32(b + 20);
    if (ec < 1) return GBL_PAYLOAD_BAD_ENTRY_COUNT;
    if ((size_t)GBLP1_HEADER_SIZE + (size_t)ec * GBLP1_ENTRY_SIZE
        + GBLP1_FOOTER_SIZE > (size_t)total)
        return GBL_PAYLOAD_BAD_ENTRY_COUNT;
    if (gbl_crc32(b, 24) != le32(b + 24))
        return GBL_PAYLOAD_HEADER_CRC_MISMATCH;
    if (memcmp(b + total - GBLP1_FOOTER_SIZE, GBLP1_FOOTER, GBLP1_FOOTER_SIZE) != 0)
        return GBL_PAYLOAD_FOOTER_MISMATCH;
    return GBL_PAYLOAD_OK;
}

#include "Internal/Sha256.h"

/* Walk + integrity-check every entry; return the unique entry whose
   type == want_type. Returns GBL_PAYLOAD_OK with *out and *out_size set,
   or an integrity error, or GBL_PAYLOAD_OK with *out == NULL when no
   entry of want_type exists (caller maps that to its own "missing").
   Unrecognized types (those that do not equal want_type) are silently
   skipped — forward-compatibility by design. */
static enum gbl_payload_status
gbl_payload_find_entry(const uint8_t *b, size_t n, uint16_t want_type,
                       const uint8_t **out, size_t *out_size) {
    enum gbl_payload_status s = gbl_payload_validate_header(b, n);
    if (s != GBL_PAYLOAD_OK) return s;

    uint32_t total = le32(b + 16);
    uint32_t ec = le32(b + 20);
    const uint8_t *entries = b + GBLP1_HEADER_SIZE;
    size_t payload_region_start =
        GBLP1_HEADER_SIZE + (size_t)ec * GBLP1_ENTRY_SIZE;
    payload_region_start = (payload_region_start + GBLP1_PAYLOAD_ALIGN - 1)
                           & ~((size_t)GBLP1_PAYLOAD_ALIGN - 1);

    int found = 0;
    const uint8_t *found_pe = NULL;
    size_t found_size = 0;

    for (uint32_t i = 0; i < ec; i++) {
        const uint8_t *e = entries + (size_t)i * GBLP1_ENTRY_SIZE;
        uint16_t type     = le16(e + 0);
        uint16_t flags    = le16(e + 2);
        uint32_t off      = le32(e + 4);
        uint32_t sz       = le32(e + 8);
        uint32_t reserved = le32(e + 12);
        const uint8_t *recorded_sha = e + 16;

        if (type == 0)     return GBL_PAYLOAD_ENTRY_BAD_TYPE;
        if (flags != 0)    return GBL_PAYLOAD_ENTRY_BAD_FLAGS;
        if (reserved != 0) return GBL_PAYLOAD_ENTRY_BAD_RESERVED;
        if (off < payload_region_start ||
            (off & (GBLP1_PAYLOAD_ALIGN - 1)) != 0)
            return GBL_PAYLOAD_ENTRY_BAD_OFFSET;
        if ((size_t)off + sz + GBLP1_FOOTER_SIZE > (size_t)total)
            return GBL_PAYLOAD_ENTRY_BAD_SIZE;

        uint8_t got[32];
        gbl_sha256(b + off, sz, got);
        if (memcmp(got, recorded_sha, 32) != 0)
            return GBL_PAYLOAD_ENTRY_SHA_MISMATCH;

        if (type == want_type) {
            if (found) return GBL_PAYLOAD_ENTRY_BAD_TYPE; /* duplicate */
            found = 1;
            found_pe = b + off;
            found_size = sz;
        }
    }

    *out      = found ? found_pe : NULL;
    *out_size = found_size;
    return GBL_PAYLOAD_OK;
}

enum gbl_payload_status
gbl_payload_find_cached_abl(const uint8_t *b, size_t n,
                            const uint8_t **out_pe, size_t *out_size) {
    const uint8_t *pe = NULL; size_t sz = 0;
    enum gbl_payload_status s =
        gbl_payload_find_entry(b, n, GBLP1_TYPE_CACHED_ABL, &pe, &sz);
    if (s != GBL_PAYLOAD_OK) return s;
    if (pe == NULL) return GBL_PAYLOAD_NO_CACHED_ABL;
    *out_pe = pe; *out_size = sz;
    return GBL_PAYLOAD_OK;
}

enum gbl_payload_status
gbl_payload_find_mode2_profile(const uint8_t *b, size_t n,
                               const uint8_t **out_profile,
                               size_t *out_size) {
    const uint8_t *p = NULL; size_t sz = 0;
    enum gbl_payload_status s =
        gbl_payload_find_entry(b, n, GBLP1_TYPE_MODE2_PROFILE, &p, &sz);
    if (s != GBL_PAYLOAD_OK) return s;
    if (p == NULL) return GBL_PAYLOAD_NO_MODE2_PROFILE;
    *out_profile = p; *out_size = sz;
    return GBL_PAYLOAD_OK;
}

enum gbl_payload_status
gbl_payload_scan_cached_abl(const uint8_t *bytes, size_t size,
                            const uint8_t **out_pe, size_t *out_pe_size) {
    enum gbl_payload_status last = GBL_PAYLOAD_BAD_MAGIC;
    int found_any = 0;

    for (size_t i = 0; i + GBLP1_MAGIC_SIZE <= size; i++) {
        if (bytes[i] != (uint8_t)'G') continue;
        if (memcmp(bytes + i, GBLP1_MAGIC, GBLP1_MAGIC_SIZE) != 0) continue;

        found_any = 1;
        enum gbl_payload_status s =
            gbl_payload_find_cached_abl(bytes + i, size - i, out_pe, out_pe_size);
        if (s == GBL_PAYLOAD_OK) return GBL_PAYLOAD_OK;
        last = s;
    }

    (void)found_any;
    return last;
}
