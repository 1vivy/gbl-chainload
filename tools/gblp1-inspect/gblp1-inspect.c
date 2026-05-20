/* tools/gblp1-inspect/gblp1-inspect.c — GBLP1 container inspector.
   Locates GBLP1 magic in an image (base-EFI prefix optional), validates
   the header CRC-32, verifies every entry's SHA-256 digest, and emits
   machine-greppable lines. Exit 0 iff the container is fully valid.

   find_container distinguishes three outcomes:
     0  — fully valid candidate found (*out_offset set)
     1  — best corrupt candidate found (*out_offset set, *out_failure_reason
            is "bad_crc" or "truncated"); no fully valid candidate exists
    -1  — no plausible candidate at all (result: not_a_gblp1)

   A "plausible" candidate matches the magic bytes AND has
   version==1 / header_size==28 / flags&1.  Candidates that fail only the
   CRC or the size bounds (including the 16 MiB total_size cap) are tracked
   as "best corrupt" so a partial-write install can be diagnosed rather than
   silently reported as "not a GBLP1 image". */
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

/* find_container — locate the best GBLP1 candidate in buf[0..len).
 *
 * Returns:
 *   0  — fully valid candidate; *out_offset is its byte offset.
 *   1  — best corrupt candidate; *out_offset is set; *out_failure_reason is
 *          "bad_crc" or "truncated".  Emitting this lets the caller report
 *          a precise diagnosis rather than a generic "not_a_gblp1".
 *  -1  — no plausible candidate at all.
 *
 * "Plausible" = magic matches AND version/header_size/flags have the
 * expected v1 values.  Occurrences that are merely embedded string literals
 * in a base-EFI prefix will fail those checks and are silently skipped.
 * Candidates that pass the basic shape check but fail the CRC or size
 * bounds (including the 16 MiB cap) are tracked as "best corrupt". */
static int find_container(const uint8_t *buf, size_t len,
                          ssize_t *out_offset,
                          const char **out_failure_reason) {
    ssize_t best_corrupt = -1;
    const char *best_reason = NULL;

    if (len < GBLP1_HEADER_SIZE) return -1;
    for (size_t i = 0; i + GBLP1_HEADER_SIZE <= len; i++) {
        if (memcmp(buf + i, GBLP1_MAGIC, GBLP1_MAGIC_SIZE) != 0)
            continue;
        /* Candidate found — check basic shape first. */
        const uint8_t *h = buf + i;
        size_t avail     = len - i;
        uint16_t version    = rle16(h + 8);
        uint16_t hdr_size   = rle16(h + 10);
        uint32_t flags      = rle32(h + 12);
        if (version  != GBLP1_VERSION)      continue; /* not a real v1 header */
        if (hdr_size != GBLP1_HEADER_SIZE)  continue;
        if ((flags & GBLP1_FLAGS_LE) == 0)  continue;
        /* Basic shape passes — this is a plausible GBLP1 header.
         * Now check size bounds (including the on-device 16 MiB cap) and CRC.
         * A failure here is diagnostically meaningful, so track as best_corrupt. */
        uint32_t total_size = rle32(h + 16);
        uint32_t hdr_crc    = rle32(h + 24);
        if (total_size > GBLP1_TOTAL_SIZE_CAP ||
            total_size > avail ||
            total_size < GBLP1_HEADER_SIZE + GBLP1_FOOTER_SIZE) {
            if (best_corrupt < 0) {
                best_corrupt = (ssize_t)i;
                best_reason  = "truncated";
            }
            continue;
        }
        if (gbl_crc32(h, 24) != hdr_crc) {
            if (best_corrupt < 0) {
                best_corrupt = (ssize_t)i;
                best_reason  = "bad_crc";
            }
            continue;
        }
        /* Fully valid. */
        *out_offset = (ssize_t)i;
        return 0;
    }
    if (best_corrupt >= 0) {
        *out_offset         = best_corrupt;
        *out_failure_reason = best_reason;
        return 1;
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
        printf("gblp1-inspect %s\n", GBL_TOOL_VERSION);
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "gblp1-inspect %s\nusage: gblp1-inspect <image>\n",
                GBL_TOOL_VERSION);
        return 2;
    }
    uint8_t *buf = NULL; size_t blen = 0;
    if (slurp(argv[1], &buf, &blen)) return 1;

    ssize_t mo = -1;
    const char *failure_reason = NULL;
    int fc = find_container(buf, blen, &mo, &failure_reason);
    if (fc < 0) { puts("result: not_a_gblp1"); free(buf); return 1; }
    if (fc > 0) {
        /* Plausible header found but corrupt — emit specific diagnosis. */
        printf("result: %s\n", failure_reason);
        free(buf); return 1;
    }

    /* find_container(rc==0) already validated: version==1, header_size==28,
       flags&1, total_size<=avail, total_size<=16MiB cap, header_crc32 matches.
       Re-read the fields we need for the entry-scan and footer check. */
    const uint8_t *h = buf + mo;
    uint16_t version     = rle16(h + 8);
    uint32_t total_size  = rle32(h + 16);
    uint32_t entry_count = rle32(h + 20);
    printf("header: magic=ok version=%u header_crc32=ok total_size=%u entry_count=%u\n",
           version, total_size, entry_count);

    /* Overflow-safe entry-table bounds check: header + entries + footer
       must fit within total_size.  Catches a header whose entry_count was
       corrupted independently of the CRC (e.g. partial write). */
    uint64_t entries_end = (uint64_t)GBLP1_HEADER_SIZE +
                           (uint64_t)entry_count * (uint64_t)GBLP1_ENTRY_SIZE;
    if (entries_end + GBLP1_FOOTER_SIZE > (uint64_t)total_size) {
        puts("result: truncated");
        free(buf); return 1;
    }

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
