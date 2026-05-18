/* GblChainloadPkg/Library/ProtocolHookLib/Mode2Rewrite.c — pure-logic
   KM/SPSS rewrite. Field offsets cross-checked against
   ~/gbl_root_canoe/tools/keymaster_wire.h and QseecomHook.c's decoder. */
#include "Mode2Rewrite.h"

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;       p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wrbytes(uint8_t *dst, const uint8_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
}

/* SET_ROT (44B):  {cmd@0, RotOffset@4, RotSize@8, RotDigest[32]@12} */
static void rewrite_set_rot(uint8_t *b, const struct gbl_mode2_profile *p) {
    wrbytes(b + 12, p->rot_digest, 32);
}
/* SET_VERSION (12B): {cmd@0, OsVersion@4, OsPatchLevel@8} */
static void rewrite_set_version(uint8_t *b, const struct gbl_mode2_profile *p) {
    wr32(b + 4, p->system_version);
    wr32(b + 8, p->system_spl);
}
/* SET_BOOT_STATE (64B): {cmd@0, Version@4, Offset@8, Size@12,
     BootState{IsUnlocked@16, PublicKey[32]@20, Color@52,
               SystemVersion@56, SystemSecurityLevel@60}} */
static void rewrite_set_boot_state(uint8_t *b,
                                   const struct gbl_mode2_profile *p) {
    wr32(b + 16, p->is_unlocked);
    wrbytes(b + 20, p->pubkey_digest, 32);
    wr32(b + 52, p->color);
    wr32(b + 56, p->system_version);
    wr32(b + 60, p->system_spl);
}
/* SET_VBH (36B): {cmd@0, Vbh[32]@4} */
static void rewrite_set_vbh(uint8_t *b, const struct gbl_mode2_profile *p) {
    wrbytes(b + 4, p->vbh, 32);
}

int gbl_m2_rewrite_km(uint32_t cmd_id, uint8_t *buf, uint32_t len,
                      const struct gbl_mode2_profile *p) {
    if (buf == NULL || p == NULL) return 0;
    switch (cmd_id) {
        case GBL_KM_CMD_SET_ROT:
            if (len != GBL_KM_LEN_SET_ROT) return 0;
            rewrite_set_rot(buf, p); return 1;
        case GBL_KM_CMD_SET_VERSION:
            if (len != GBL_KM_LEN_SET_VERSION) return 0;
            rewrite_set_version(buf, p); return 1;
        case GBL_KM_CMD_SET_BOOT_STATE:
            if (len != GBL_KM_LEN_SET_BOOT_STATE) return 0;
            rewrite_set_boot_state(buf, p); return 1;
        case GBL_KM_CMD_SET_VBH:
            if (len != GBL_KM_LEN_SET_VBH) return 0;
            rewrite_set_vbh(buf, p); return 1;
        default:
            return 0;
    }
}

int gbl_m2_rewrite_spss(uint8_t *info, uint32_t info_len,
                        const struct gbl_mode2_profile *p) {
    if (info == NULL || p == NULL || info_len < GBL_SPSS_INFO_LEN) return 0;
    rewrite_set_rot(info + 0, p);          /* RoT sub-struct  @0  (44) */
    rewrite_set_boot_state(info + 44, p);  /* BootState       @44 (64) */
    rewrite_set_vbh(info + 108, p);        /* Vbh             @108(36) */
    return 1;
}
