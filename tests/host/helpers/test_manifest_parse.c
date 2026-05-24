/* tests/host/helpers/test_manifest_parse.c — exercises
   gbl_payload_find_manifest() across absence, valid, and malformed
   manifest entries (Task 1 of the engine-rework PR1). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
/* PR2 Task 4: parser + sha256 + crc32 now ship in libgblp1.a
 * (crates/gblp1). One header replaces the three deleted
 * Internal/PayloadParse.h, Internal/Sha256.h, Internal/Crc32.h. */
#include "../../../crates/gblp1/include/gblp1_ffi.h"

static void wle16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }
static void wle32(uint8_t *p, uint32_t v) {
    p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24;
}
static uint32_t rle32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Build a minimal valid GBLP1 container with one manifest entry whose
   payload bytes are caller-supplied. `entry_size` is the size recorded
   in the entry header (and used to bound the SHA + memcpy) — pass 16
   for spec-conformant manifests; other values exercise the
   BAD_MANIFEST_SIZE path. `payload_buf_size` is how many bytes the
   caller supplied (only used to clamp the memcpy). Returns alloc'd
   buffer + size; caller frees. */
static uint8_t *make_container(const uint8_t *payload, size_t payload_buf_size,
                               size_t entry_size, size_t *out_size) {
    uint32_t entries_end = GBLP1_HEADER_SIZE + GBLP1_ENTRY_SIZE;
    uint32_t off = (entries_end + GBLP1_PAYLOAD_ALIGN - 1)
                   & ~(GBLP1_PAYLOAD_ALIGN - 1);
    uint32_t total = off + (uint32_t)entry_size + GBLP1_FOOTER_SIZE;
    /* Align footer for cleanliness. */
    total = (total + GBLP1_PAYLOAD_ALIGN - 1) & ~(GBLP1_PAYLOAD_ALIGN - 1);
    uint8_t *buf = calloc(1, total);
    memcpy(buf, GBLP1_MAGIC, GBLP1_MAGIC_SIZE);
    wle16(buf + 8,  GBLP1_VERSION);
    wle16(buf + 10, GBLP1_HEADER_SIZE);
    wle32(buf + 12, GBLP1_FLAGS_LE);
    wle32(buf + 16, total);
    wle32(buf + 20, 1);
    uint8_t *e = buf + GBLP1_HEADER_SIZE;
    wle16(e + 0,  GBLP1_TYPE_MANIFEST);
    wle32(e + 4,  off);
    wle32(e + 8,  (uint32_t)entry_size);
    size_t copy = payload_buf_size < entry_size ? payload_buf_size : entry_size;
    if (payload && copy) memcpy(buf + off, payload, copy);
    gbl_sha256(buf + off, entry_size, e + 16);
    memcpy(buf + total - GBLP1_FOOTER_SIZE, GBLP1_FOOTER, GBLP1_FOOTER_SIZE);
    wle32(buf + 24, gbl_crc32(buf, 24));
    *out_size = total;
    return buf;
}

/* Build a 16-byte manifest payload with the requested deviations. */
static void make_payload(uint8_t *out16, uint16_t cap_bits, uint16_t schema_ver,
                         int bad_magic, int bad_pad) {
    memset(out16, 0, 16);
    memcpy(out16, bad_magic ? "BAD!" : GBLP1_MANIFEST_MAGIC, 4);
    wle16(out16 + 4, schema_ver);
    wle16(out16 + 6, cap_bits);
    if (bad_pad) out16[15] = 0xff;
}

#define CASE(label, expected_status, expected_present, expected_bits, \
             bits, schema, bad_magic, bad_pad) do { \
    uint8_t pl[16]; make_payload(pl, (bits), (schema), (bad_magic), (bad_pad)); \
    size_t n; uint8_t *b = make_container(pl, sizeof(pl), sizeof(pl), &n); \
    struct gbl_manifest m = {0}; int present = -1; \
    enum gbl_payload_status s = gbl_payload_find_manifest(b, n, &m, &present); \
    if (s != (expected_status) || \
        ((expected_status) == GBL_PAYLOAD_OK && present != (expected_present)) || \
        ((expected_status) == GBL_PAYLOAD_OK && present && \
         m.cap_bits != (expected_bits))) { \
        fprintf(stderr, "FAIL %s: status=%d present=%d bits=0x%x\n", \
                (label), (int)s, present, m.cap_bits); \
        free(b); return 1; \
    } \
    free(b); pass++; \
} while (0)

int main(void) {
    int pass = 0;

    /* Valid: mode-0 (no bits), mode-1 (fakelock only), mode-2 (profile only). */
    CASE("mode-0",    GBL_PAYLOAD_OK,                    1, 0x0000,
         0x0000, 1, 0, 0);
    CASE("mode-1",    GBL_PAYLOAD_OK,                    1, 0x0001,
         0x0001, 1, 0, 0);
    CASE("mode-2",    GBL_PAYLOAD_OK,                    1, 0x0002,
         0x0002, 1, 0, 0);

    /* Malformed: bad magic, bad schema, reserved bit set, non-zero pad. */
    CASE("bad-magic", GBL_PAYLOAD_BAD_MANIFEST_MAGIC,    0, 0,
         0x0000, 1, 1, 0);
    CASE("bad-sch",   GBL_PAYLOAD_BAD_MANIFEST_SCHEMA,   0, 0,
         0x0000, 2, 0, 0);
    CASE("bad-bit",   GBL_PAYLOAD_BAD_MANIFEST_RESERVED, 0, 0,
         0x0004, 1, 0, 0);
    CASE("bad-pad",   GBL_PAYLOAD_BAD_MANIFEST_RESERVED, 0, 0,
         0x0000, 1, 0, 1);

    /* Absence: container with a non-manifest entry (cached-ABL). */
    {
        uint8_t pl[16] = {0};
        size_t n; uint8_t *b = make_container(pl, 16, 16, &n);
        uint8_t *e = b + GBLP1_HEADER_SIZE;
        wle16(e + 0, GBLP1_TYPE_CACHED_ABL);   /* re-type to non-manifest */
        uint32_t off = rle32(e + 4);
        gbl_sha256(b + off, 16, e + 16);       /* SHA unchanged (payload same) */
        wle32(b + 24, gbl_crc32(b, 24));       /* header CRC unchanged in fact */
        struct gbl_manifest m = {0}; int present = -1;
        enum gbl_payload_status s =
            gbl_payload_find_manifest(b, n, &m, &present);
        if (s != GBL_PAYLOAD_OK || present != 0) {
            fprintf(stderr, "FAIL absence: status=%d present=%d\n",
                    (int)s, present);
            free(b); return 1;
        }
        free(b); pass++;
    }

    /* Bad manifest size: entry recorded as 15 bytes (spec requires 16). */
    {
        uint8_t pl[16] = {0};
        make_payload(pl, 0x0000, 1, 0, 0);
        size_t n; uint8_t *b = make_container(pl, sizeof(pl), 15, &n);
        struct gbl_manifest m = {0}; int present = -1;
        enum gbl_payload_status s =
            gbl_payload_find_manifest(b, n, &m, &present);
        if (s != GBL_PAYLOAD_BAD_MANIFEST_SIZE) {
            fprintf(stderr, "FAIL bad-size: status=%d present=%d\n",
                    (int)s, present);
            free(b); return 1;
        }
        free(b); pass++;
    }

    printf("093_manifest_parse: OK (%d/9)\n", pass);
    return pass == 9 ? 0 : 1;
}
