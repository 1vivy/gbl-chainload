/* GblChainloadPkg/Library/GblPayloadLib/PayloadParse.c — pure-logic
   parser. The EDK2 IO wrapper (LocateOverlay.c, EfispBlockIo.c) calls
   into this with a ready-to-validate byte buffer. */
#include <string.h>
#include "Internal/PayloadParse.h"
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
