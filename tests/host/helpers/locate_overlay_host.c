/* tests/host/helpers/locate_overlay_host.c
   Host-side simulation of LocateOverlay's decision: if cfg table has the
   right magic+version, return its (Base, Size); else return the BlockIO
   source. The same decision logic ports to EDK2 in LocateOverlay.c. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define GBLS_MAGIC 0x534C4247u  /* 'G''B''L''S' little-endian */

struct cfg {
    uint32_t magic;
    uint32_t version;
    const uint8_t *base;
    size_t size;
};

static int locate(const struct cfg *table_or_null,
                  const uint8_t *blockio_bytes, size_t blockio_size,
                  const uint8_t **out_bytes, size_t *out_size,
                  const char **out_origin) {
    if (table_or_null && table_or_null->magic == GBLS_MAGIC &&
        table_or_null->version == 1) {
        *out_bytes = table_or_null->base;
        *out_size = table_or_null->size;
        *out_origin = "staged-buffer";
        return 0;
    }
    if (blockio_bytes && blockio_size > 0) {
        *out_bytes = blockio_bytes;
        *out_size = blockio_size;
        *out_origin = "efisp-blockio";
        return 0;
    }
    return 1;
}

int main(void) {
    uint8_t a[16] = "AAAA"; uint8_t b[16] = "BBBB";
    const uint8_t *out; size_t sz; const char *origin;

    /* Both sources present → cfg wins. */
    struct cfg t = {GBLS_MAGIC, 1, a, 16};
    if (locate(&t, b, 16, &out, &sz, &origin) != 0) return 1;
    if (strcmp(origin, "staged-buffer") != 0) {
        fprintf(stderr, "FAIL: cfg should win\n"); return 1;
    }
    /* Bad magic in cfg → blockio wins. */
    struct cfg bad = {0xDEADu, 1, a, 16};
    if (locate(&bad, b, 16, &out, &sz, &origin) != 0) return 1;
    if (strcmp(origin, "efisp-blockio") != 0) {
        fprintf(stderr, "FAIL: blockio should win on bad cfg magic\n"); return 1;
    }
    /* Wrong version → blockio wins. */
    struct cfg badv = {GBLS_MAGIC, 2, a, 16};
    if (locate(&badv, b, 16, &out, &sz, &origin) != 0) return 1;
    if (strcmp(origin, "efisp-blockio") != 0) {
        fprintf(stderr, "FAIL: blockio should win on bad cfg version\n"); return 1;
    }
    /* No cfg, no blockio → fail. */
    if (locate(NULL, NULL, 0, &out, &sz, &origin) != 1) {
        fprintf(stderr, "FAIL: should fail with no source\n"); return 1;
    }
    printf("PASS: locate_overlay\n");
    return 0;
}
