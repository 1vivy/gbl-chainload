/* tools/shared/gblp1.h — GBLP1 v1 container layout (LE).
   Shared between EDK2 GblPayloadLib and host tools/gbl-pack. */
#ifndef GBLP1_H_
#define GBLP1_H_

#include <stdint.h>

#define GBLP1_MAGIC          "GBLP1\0\0\0"
#define GBLP1_MAGIC_SIZE     8u
#define GBLP1_VERSION        0x0001u
#define GBLP1_HEADER_SIZE    28u
#define GBLP1_FLAGS_LE       0x00000001u
#define GBLP1_FOOTER         "GBLP1END"
#define GBLP1_FOOTER_SIZE    8u
#define GBLP1_TOTAL_SIZE_CAP (16u * 1024u * 1024u)
#define GBLP1_PAYLOAD_ALIGN  16u

#define GBLP1_TYPE_CACHED_ABL    0x0001u
#define GBLP1_TYPE_SOURCE_META   0x0002u
#define GBLP1_TYPE_MODE2_PROFILE 0x0010u  /* mode-2 profile (GM2P) */
#define GBLP1_TYPE_MANIFEST      0x0020u  /* engine capability manifest (GMAN) */

/* Manifest payload (16 bytes; little-endian).
   Layout: magic[4] | schema_version u16 | cap_bits u16 | reserved_pad[8] */
#define GBLP1_MANIFEST_MAGIC                "GMAN"
#define GBLP1_MANIFEST_MAGIC_SIZE           4u
#define GBLP1_MANIFEST_SIZE                 16u
#define GBLP1_MANIFEST_SCHEMA_VERSION       1u
#define GBLP1_MANIFEST_BIT_FAKELOCK_HOOK    0x0001u
#define GBLP1_MANIFEST_BIT_PROFILE_SPOOF    0x0002u
#define GBLP1_MANIFEST_BITS_RESERVED_MASK   0xFFFCu  /* bits 2..15 must be 0 */

#define GBLP1_ENTRY_SIZE     48u

/* On-disk header — must be packed and LE. */
struct gblp1_header {
    uint8_t  magic[8];        /* "GBLP1\0\0\0" */
    uint16_t version;         /* 1 */
    uint16_t header_size;     /* 28 */
    uint32_t flags;           /* bit0 = LE marker */
    uint32_t total_size;      /* entire container */
    uint32_t entry_count;     /* >= 1 */
    uint32_t header_crc32;    /* CRC32 over bytes [0..24) */
};

struct gblp1_entry {
    uint16_t type;
    uint16_t flags;           /* must be 0 in v1 */
    uint32_t payload_offset;  /* absolute, 16-byte aligned */
    uint32_t payload_size;
    uint32_t reserved;        /* must be 0 */
    uint8_t  sha256[32];
};

/* Compile-time size guards. */
_Static_assert(sizeof(struct gblp1_header) == GBLP1_HEADER_SIZE,
               "gblp1_header must be 28 bytes packed");
_Static_assert(sizeof(struct gblp1_entry) == GBLP1_ENTRY_SIZE,
               "gblp1_entry must be 48 bytes packed");

#endif /* GBLP1_H_ */
