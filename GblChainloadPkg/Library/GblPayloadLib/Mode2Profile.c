/* GblChainloadPkg/Library/GblPayloadLib/Mode2Profile.c — pure-logic
   mode2_profile parser. No EDK2 / libc dependency beyond the byte
   helpers below, so it builds host-side (GBL_HOST_BUILD) and in EDK2. */
#include "Internal/Mode2Profile.h"

static uint16_t m2p_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t m2p_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int m2p_memeq(const uint8_t *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (a[i] != (uint8_t)b[i]) return 0;
    return 1;
}
static void m2p_copy(uint8_t *dst, const uint8_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
}

enum gbl_m2p_status
gbl_mode2_profile_parse(const uint8_t *b, size_t n,
                        struct gbl_mode2_profile *out) {
    if (b == NULL || out == NULL)            return GBL_M2P_BAD_FIELD;
    if (n != (size_t)GBL_M2P_SIZE)           return GBL_M2P_TOO_SMALL;
    if (!m2p_memeq(b, GBL_M2P_MAGIC, GBL_M2P_MAGIC_SIZE))
                                             return GBL_M2P_BAD_MAGIC;
    if (m2p_le16(b + 4) != GBL_M2P_VERSION)  return GBL_M2P_BAD_VERSION;
    if (m2p_le16(b + 6) != 0)                return GBL_M2P_BAD_RESERVED;

    uint32_t is_unlocked = m2p_le32(b + 8);
    uint32_t color       = m2p_le32(b + 12);
    if (is_unlocked > 1u)                    return GBL_M2P_BAD_FIELD;
    if (color > GBL_M2P_COLOR_RED)           return GBL_M2P_BAD_FIELD;

    m2p_copy(out->magic, b + 0, 4);
    out->version        = m2p_le16(b + 4);
    out->reserved       = 0;
    out->is_unlocked    = is_unlocked;
    out->color          = color;
    out->system_version = m2p_le32(b + 16);
    out->system_spl     = m2p_le32(b + 20);
    m2p_copy(out->rot_digest,    b + 24, 32);
    m2p_copy(out->pubkey_digest, b + 56, 32);
    m2p_copy(out->vbh,           b + 88, 32);
    return GBL_M2P_OK;
}
