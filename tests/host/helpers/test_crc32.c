/* tests/host/helpers/test_crc32.c
   Known-answer test for the vendored gbl_crc32.

   These vectors pin the implementation to IEEE 802.3 CRC-32, which is
   bit-identical to zlib crc32() and to EDK2's CalculateCrc32 — so the
   GBLP1 header CRC written by gbl-pack and checked by the EFI shim agree
   by construction. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../../GblChainloadPkg/Library/GblPayloadLib/Internal/Crc32.h"

struct kat {
    const char *name;
    const char *msg;       /* NUL-terminated; length taken with strlen */
    uint32_t    crc;
};

static const struct kat vectors[] = {
    { "empty",      "",                                              0x00000000u },
    { "check",      "123456789",                                     0xCBF43926u },
    { "fox",        "The quick brown fox jumps over the lazy dog",    0x414FA339u },
};

int main(void) {
    int failed = 0;
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        const struct kat *v = &vectors[i];
        uint32_t got = gbl_crc32((const uint8_t *)v->msg, strlen(v->msg));
        if (got != v->crc) {
            fprintf(stderr, "FAIL: crc32(%s) expected 0x%08x, got 0x%08x\n",
                    v->name, v->crc, got);
            failed = 1;
        }
    }
    if (failed) return 1;
    printf("PASS: crc32 (%zu IEEE 802.3 vectors)\n",
           sizeof(vectors) / sizeof(vectors[0]));
    return 0;
}
