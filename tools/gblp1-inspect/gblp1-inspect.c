/* tools/gblp1-inspect/gblp1-inspect.c — GBLP1 container inspector.
   Locates GBLP1 magic in an image (base-EFI prefix optional), validates
   the header CRC-32, verifies every entry's SHA-256 digest, and emits
   machine-greppable lines. Exit 0 iff the container is fully valid.
   find_magic skips any magic occurrence whose header is not structurally
   valid (version==1, header_size==28, flags&1, total_size<=avail, CRC ok)
   so that embedded string literals in a base-EFI prefix are ignored. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../shared/gblp1.h"
#include "Internal/Sha256.h"
#include "Internal/Crc32.h"

static int slurp(const char *path, uint8_t **out, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fprintf(stderr, "%s: empty\n", path); fclose(f); return 1; }
    uint8_t *b = malloc((size_t)n);
    if (!b) { fclose(f); return 1; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return 1; }
    fclose(f); *out = b; *out_size = (size_t)n; return 0;
}

static uint16_t rle16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rle32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const char *type_name(uint16_t t) {
    switch (t) {
        case GBLP1_TYPE_CACHED_ABL:    return "CACHED_ABL";
        case GBLP1_TYPE_SOURCE_META:   return "SOURCE_META";
        case GBLP1_TYPE_MODE2_PROFILE: return "MODE2_PROFILE";
        default:                       return "UNKNOWN";
    }
}

/* find_magic — return the offset of the first GBLP1 magic occurrence whose
   header fields are all structurally valid.  Occurrences that are embedded
   string constants (e.g. inside a base-EFI prefix) will fail the version /
   header_size / flags / total_size / CRC checks and are silently skipped. */
static ssize_t find_magic(const uint8_t *buf, size_t len) {
    if (len < GBLP1_HEADER_SIZE) return -1;
    for (size_t i = 0; i + GBLP1_HEADER_SIZE <= len; i++) {
        if (memcmp(buf + i, GBLP1_MAGIC, GBLP1_MAGIC_SIZE) != 0)
            continue;
        /* Candidate found — validate header fields before committing. */
        const uint8_t *h = buf + i;
        size_t avail = len - i;
        uint16_t version    = rle16(h + 8);
        uint16_t hdr_size   = rle16(h + 10);
        uint32_t flags      = rle32(h + 12);
        uint32_t total_size = rle32(h + 16);
        uint32_t hdr_crc    = rle32(h + 24);
        if (version != GBLP1_VERSION)           continue;
        if (hdr_size != GBLP1_HEADER_SIZE)      continue;
        if ((flags & GBLP1_FLAGS_LE) == 0)      continue;
        if (total_size > avail)                  continue;
        if (total_size < GBLP1_HEADER_SIZE + GBLP1_FOOTER_SIZE) continue;
        if (gbl_crc32(h, 24) != hdr_crc)        continue;
        return (ssize_t)i;
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: gblp1-inspect <image>\n"); return 2; }
    uint8_t *buf = NULL; size_t blen = 0;
    if (slurp(argv[1], &buf, &blen)) return 1;

    ssize_t mo = find_magic(buf, blen);
    if (mo < 0) { puts("result: not_a_gblp1"); free(buf); return 1; }

    /* find_magic already validated: version==1, header_size==28, flags&1,
       total_size<=avail, header_crc32 matches.  Re-read the fields we need
       for the entry-scan and footer check. */
    const uint8_t *h = buf + mo;
    uint16_t version     = rle16(h + 8);
    uint32_t total_size  = rle32(h + 16);
    uint32_t entry_count = rle32(h + 20);
    printf("header: magic=ok version=%u header_crc32=ok total_size=%u entry_count=%u\n",
           version, total_size, entry_count);

    int sha_fail = 0;
    for (uint32_t i = 0; i < entry_count; i++) {
        const uint8_t *e = h + GBLP1_HEADER_SIZE + i * GBLP1_ENTRY_SIZE;
        uint16_t type    = rle16(e + 0);
        uint32_t poff    = rle32(e + 4);
        uint32_t psize   = rle32(e + 8);
        const uint8_t *want_sha = e + 16;
        if (psize > total_size || poff > total_size - psize) {
            printf("entry: type=0x%04x (%s) offset=0x%x size=%u sha256=OUT_OF_BOUNDS\n",
                   type, type_name(type), poff, psize);
            sha_fail = 1; continue;
        }
        uint8_t got_sha[32];
        gbl_sha256(h + poff, psize, got_sha);
        int ok = memcmp(got_sha, want_sha, 32) == 0;
        printf("entry: type=0x%04x (%s) offset=0x%x size=%u sha256=%s\n",
               type, type_name(type), poff, psize, ok ? "ok" : "MISMATCH");
        if (!ok) sha_fail = 1;
    }

    const uint8_t *foot = h + total_size - GBLP1_FOOTER_SIZE;
    int footer_ok = memcmp(foot, GBLP1_FOOTER, GBLP1_FOOTER_SIZE) == 0;
    printf("footer: GBLP1END=%s\n", footer_ok ? "ok" : "MISSING");

    free(buf);
    if (!footer_ok) { puts("result: truncated"); return 1; }
    if (sha_fail)   { puts("result: entry_sha_mismatch"); return 1; }
    puts("result: ok");
    return 0;
}
