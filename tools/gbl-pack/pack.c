/* tools/gbl-pack/pack.c — pure-logic GBLP1 packer. */
#include <stdlib.h>
#include <string.h>
#include "pack.h"
#include "../shared/gblp1.h"
#include "../shared/efisp_scan.h"
#include "../shared/gbl_mode2_profile.h"
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
    if (!in)
        return GBL_PACK_ERR_BAD_INPUT;
    int have_cached  = (in->cached_abl && in->cached_abl_size > 0);
    int have_profile = (in->mode2_profile && in->mode2_profile_size > 0);
    if (!have_cached && !have_profile)
        return GBL_PACK_ERR_BAD_INPUT;

    if (have_cached) {
        if (gbl_contains_utf16_efisp(in->cached_abl, in->cached_abl_size))
            return GBL_PACK_ERR_EFISP_PRESENT;
        if (gbl_pe_sanity(in->cached_abl, in->cached_abl_size) != GBL_PE_OK)
            return GBL_PACK_ERR_PE_INSANE;
    }
    if (have_profile) {
        if (in->mode2_profile_size != GBL_M2P_SIZE)
            return GBL_PACK_ERR_PROFILE_BAD;
        if (memcmp(in->mode2_profile, GBL_M2P_MAGIC, 4) != 0)
            return GBL_PACK_ERR_PROFILE_BAD;
    }

    /* source_meta payload (only emitted alongside cached_abl). */
    size_t pv_len = in->packer_version ? strlen(in->packer_version) : 0;
    size_t ts_len = in->timestamp_iso8601 ? strlen(in->timestamp_iso8601) : 0;
    size_t meta_size = 3 * (4 + 32) + 4 + pv_len + 4 + ts_len;

    /* Entry descriptors, in emission order. */
    struct { uint16_t type; const uint8_t *data; size_t size; } ents[3];
    uint32_t ec = 0;
    if (have_cached) {
        ents[ec].type = GBLP1_TYPE_CACHED_ABL;
        ents[ec].data = in->cached_abl; ents[ec].size = in->cached_abl_size; ec++;
        ents[ec].type = GBLP1_TYPE_SOURCE_META;
        ents[ec].data = NULL;          ents[ec].size = meta_size;            ec++;
    }
    if (have_profile) {
        ents[ec].type = GBLP1_TYPE_MODE2_PROFILE;
        ents[ec].data = in->mode2_profile; ents[ec].size = in->mode2_profile_size; ec++;
    }

    uint32_t entries_end = GBLP1_HEADER_SIZE + ec * GBLP1_ENTRY_SIZE;
    uint32_t off = align_up(entries_end, GBLP1_PAYLOAD_ALIGN);
    uint32_t payload_off[3];
    for (uint32_t i = 0; i < ec; i++) {
        payload_off[i] = off;
        off = align_up(off + (uint32_t)ents[i].size, GBLP1_PAYLOAD_ALIGN);
    }
    uint32_t total = off + GBLP1_FOOTER_SIZE;
    if (total > GBLP1_TOTAL_SIZE_CAP) return GBL_PACK_ERR_TOO_LARGE;

    uint8_t *buf = calloc(1, total);
    if (!buf) return GBL_PACK_ERR_OOM;

    /* Header. */
    memcpy(buf + 0, GBLP1_MAGIC, GBLP1_MAGIC_SIZE);
    wle16(buf + 8,  GBLP1_VERSION);
    wle16(buf + 10, GBLP1_HEADER_SIZE);
    wle32(buf + 12, GBLP1_FLAGS_LE);
    wle32(buf + 16, total);
    wle32(buf + 20, ec);

    /* Entry table + payloads. */
    for (uint32_t i = 0; i < ec; i++) {
        uint8_t *e = buf + GBLP1_HEADER_SIZE + i * GBLP1_ENTRY_SIZE;
        wle16(e + 0,  ents[i].type);
        wle16(e + 2,  0);
        wle32(e + 4,  payload_off[i]);
        wle32(e + 8,  (uint32_t)ents[i].size);
        wle32(e + 12, 0);
        if (ents[i].type == GBLP1_TYPE_SOURCE_META) {
            uint8_t *m = buf + payload_off[i];
            wle32(m, (uint32_t)in->source_size);    m += 4;
            if (in->source) gbl_sha256(in->source, in->source_size, m);
            m += 32;
            wle32(m, (uint32_t)in->extracted_size); m += 4;
            if (in->extracted) gbl_sha256(in->extracted, in->extracted_size, m);
            m += 32;
            wle32(m, (uint32_t)in->cached_abl_size); m += 4;
            gbl_sha256(in->cached_abl, in->cached_abl_size, m); m += 32;
            wle32(m, (uint32_t)pv_len);             m += 4;
            if (pv_len) memcpy(m, in->packer_version, pv_len);
            m += pv_len;
            wle32(m, (uint32_t)ts_len);             m += 4;
            if (ts_len) memcpy(m, in->timestamp_iso8601, ts_len);
        } else {
            memcpy(buf + payload_off[i], ents[i].data, ents[i].size);
        }
        gbl_sha256(buf + payload_off[i], ents[i].size, e + 16);
    }

    /* Footer + header CRC. */
    memcpy(buf + total - GBLP1_FOOTER_SIZE, GBLP1_FOOTER, GBLP1_FOOTER_SIZE);
    wle32(buf + 24, gbl_crc32(buf, 24));

    *out_buf  = buf;
    *out_size = total;
    return GBL_PACK_OK;
}
