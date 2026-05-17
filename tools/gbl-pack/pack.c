/* tools/gbl-pack/pack.c — pure-logic GBLP1 packer. */
#include <stdlib.h>
#include <string.h>
#include "pack.h"
#include "../shared/gblp1.h"
#include "../shared/efisp_scan.h"
#include "../../GblChainloadPkg/Library/GblPayloadLib/Internal/Sha256.h"
#include "../../GblChainloadPkg/Library/GblPayloadLib/Internal/Crc32.h"
#include "../../GblChainloadPkg/Library/GblPayloadLib/Internal/PeSanity.h"

static void wle16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wle32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

enum gbl_pack_status
gbl_pack_build(const struct gbl_pack_inputs *in,
               uint8_t **out_buf, size_t *out_size)
{
    if (!in || !in->cached_abl || in->cached_abl_size == 0)
        return GBL_PACK_ERR_BAD_INPUT;
    if (gbl_contains_utf16_efisp(in->cached_abl, in->cached_abl_size))
        return GBL_PACK_ERR_EFISP_PRESENT;
    if (gbl_pe_sanity(in->cached_abl, in->cached_abl_size) != GBL_PE_OK)
        return GBL_PACK_ERR_PE_INSANE;

    /* Build source_meta payload size.
       Layout: [u32 src_size][sha256 src][u32 ext_size][sha256 ext]
               [u32 abl_size][sha256 abl][u32 pv_len][pv_bytes]
               [u32 ts_len][ts_bytes]
       = 3*(4+32) + 4+pv_len + 4+ts_len */
    size_t pv_len = in->packer_version ? strlen(in->packer_version) : 0;
    size_t ts_len = in->timestamp_iso8601 ? strlen(in->timestamp_iso8601) : 0;
    size_t meta_size = 3 * (4 + 32) + 4 + pv_len + 4 + ts_len;

    uint32_t entry_count = 2; /* cached_abl + source_meta */
    uint32_t entries_end = GBLP1_HEADER_SIZE + entry_count * GBLP1_ENTRY_SIZE;
    uint32_t payload_start = align_up(entries_end, GBLP1_PAYLOAD_ALIGN);

    uint32_t cached_off = payload_start;
    uint32_t cached_end = cached_off + (uint32_t)in->cached_abl_size;
    uint32_t meta_off   = align_up(cached_end, GBLP1_PAYLOAD_ALIGN);
    uint32_t meta_end   = meta_off + (uint32_t)meta_size;
    uint32_t total      = align_up(meta_end, GBLP1_PAYLOAD_ALIGN) + GBLP1_FOOTER_SIZE;

    if (total > GBLP1_TOTAL_SIZE_CAP) return GBL_PACK_ERR_TOO_LARGE;

    uint8_t *buf = calloc(1, total);
    if (!buf) return GBL_PACK_ERR_OOM;

    /* --- Header (28 bytes) --- */
    memcpy(buf + 0, GBLP1_MAGIC, GBLP1_MAGIC_SIZE);
    wle16(buf + 8,  GBLP1_VERSION);
    wle16(buf + 10, GBLP1_HEADER_SIZE);
    wle32(buf + 12, GBLP1_FLAGS_LE);
    wle32(buf + 16, total);
    wle32(buf + 20, entry_count);
    /* buf[24..27] = header_crc32, filled last */

    /* --- Entry 0: cached_abl --- */
    uint8_t *e0 = buf + GBLP1_HEADER_SIZE;
    wle16(e0 + 0,  GBLP1_TYPE_CACHED_ABL);
    wle16(e0 + 2,  0);                          /* flags = 0 */
    wle32(e0 + 4,  cached_off);
    wle32(e0 + 8,  (uint32_t)in->cached_abl_size);
    wle32(e0 + 12, 0);                           /* reserved */
    /* e0[16..47] = sha256, filled after payload copy */

    /* --- Entry 1: source_meta --- */
    uint8_t *e1 = e0 + GBLP1_ENTRY_SIZE;
    wle16(e1 + 0,  GBLP1_TYPE_SOURCE_META);
    wle16(e1 + 2,  0);
    wle32(e1 + 4,  meta_off);
    wle32(e1 + 8,  (uint32_t)meta_size);
    wle32(e1 + 12, 0);
    /* e1[16..47] = sha256, filled after meta build */

    /* --- Payload: cached_abl --- */
    memcpy(buf + cached_off, in->cached_abl, in->cached_abl_size);

    /* --- Payload: source_meta --- */
    uint8_t *m = buf + meta_off;
    wle32(m, (uint32_t)in->source_size);    m += 4;
    if (in->source)
        gbl_sha256(in->source, in->source_size, m);
    m += 32;
    wle32(m, (uint32_t)in->extracted_size); m += 4;
    if (in->extracted)
        gbl_sha256(in->extracted, in->extracted_size, m);
    m += 32;
    wle32(m, (uint32_t)in->cached_abl_size); m += 4;
    gbl_sha256(in->cached_abl, in->cached_abl_size, m); m += 32;
    wle32(m, (uint32_t)pv_len);             m += 4;
    if (pv_len) memcpy(m, in->packer_version, pv_len);
    m += pv_len;
    wle32(m, (uint32_t)ts_len);             m += 4;
    if (ts_len) memcpy(m, in->timestamp_iso8601, ts_len);

    /* --- Per-entry SHA-256 --- */
    gbl_sha256(buf + cached_off, in->cached_abl_size, e0 + 16);
    gbl_sha256(buf + meta_off,   meta_size,            e1 + 16);

    /* --- Footer --- */
    memcpy(buf + total - GBLP1_FOOTER_SIZE, GBLP1_FOOTER, GBLP1_FOOTER_SIZE);

    /* --- Header CRC32 last (covers bytes [0..24)) --- */
    wle32(buf + 24, gbl_crc32(buf, 24));

    *out_buf  = buf;
    *out_size = total;
    return GBL_PACK_OK;
}
