/* tools/shared/gbl_mode2_profile.h — mode2_profile binary layout (LE).
   Rides in the GBLP1 container as the type 0x0010 entry payload.
   Shared between EDK2 GblPayloadLib and host tools. */
#ifndef GBL_MODE2_PROFILE_H_
#define GBL_MODE2_PROFILE_H_

#include <stdint.h>

#define GBL_M2P_MAGIC        "GM2P"
#define GBL_M2P_MAGIC_SIZE   4u
#define GBL_M2P_VERSION      0x0001u
#define GBL_M2P_SIZE         120u

/* color field values (KMBootState.Color domain) */
#define GBL_M2P_COLOR_GREEN  0u
#define GBL_M2P_COLOR_YELLOW 1u
#define GBL_M2P_COLOR_ORANGE 2u
#define GBL_M2P_COLOR_RED    3u

/* On-disk profile — packed, little-endian. Field offsets are naturally
   aligned; `packed` is belt-and-suspenders against odd ABIs. */
struct gbl_mode2_profile {
    uint8_t  magic[4];          /* "GM2P"                         off 0  */
    uint16_t version;           /* 1                              off 4  */
    uint16_t reserved;          /* 0                              off 6  */
    uint32_t is_unlocked;       /* 0 (locked) — SET_BOOT_STATE    off 8  */
    uint32_t color;             /* 0 = GREEN — SET_BOOT_STATE     off 12 */
    uint32_t system_version;    /* bootloader-domain OS version   off 16 */
    uint32_t system_spl;        /* bootloader-domain SPL          off 20 */
    uint8_t  rot_digest[32];    /* SET_ROT RotDigest              off 24 */
    uint8_t  pubkey_digest[32]; /* SET_BOOT_STATE PublicKey       off 56 */
    uint8_t  vbh[32];           /* SET_VBH Vbh                    off 88 */
} __attribute__((packed));

_Static_assert(sizeof(struct gbl_mode2_profile) == GBL_M2P_SIZE,
               "gbl_mode2_profile must be 120 bytes packed");

#endif /* GBL_MODE2_PROFILE_H_ */
