/* tests/host/helpers/test_sha256.c
   FIPS 180-4 known-answer test for the vendored gbl_sha256.

   These vectors pin the vendored implementation to the SHA-256 standard,
   which is what makes it interchangeable with any other conforming
   SHA-256 (EDK2 BaseCryptLib, host libcrypto, ...). The same source file
   is compiled into the EFI shim and the host/Android gbl-pack, so a pass
   here means producer and consumer hash identically. */
#include <stdio.h>
#include <string.h>
#include "../../../GblChainloadPkg/Library/GblPayloadLib/Internal/Sha256.h"

struct kat {
    const char *name;
    const char *msg;       /* NUL-terminated; length taken with strlen */
    uint8_t     digest[32];
};

/* FIPS 180-4 / standard SHA-256 vectors. */
static const struct kat vectors[] = {
    { "empty", "", {
        0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,
        0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
        0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,
        0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55 } },
    { "abc", "abc", {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
        0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
        0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad } },
    /* 56-byte message — crosses a block boundary (length-padding case). */
    { "two-block",
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", {
        0x24,0x8d,0x6a,0x61,0xd2,0x06,0x38,0xb8,
        0xe5,0xc0,0x26,0x93,0x0c,0x3e,0x60,0x39,
        0xa3,0x3c,0xe4,0x59,0x64,0xff,0x21,0x67,
        0xf6,0xec,0xed,0xd4,0x19,0xdb,0x06,0xc1 } },
};

int main(void) {
    int failed = 0;
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        const struct kat *v = &vectors[i];
        uint8_t got[32];
        gbl_sha256((const uint8_t *)v->msg, strlen(v->msg), got);
        if (memcmp(got, v->digest, 32) != 0) {
            fprintf(stderr, "FAIL: sha256(%s) mismatch\n", v->name);
            failed = 1;
        }
    }
    if (failed) return 1;
    printf("PASS: sha256 (%zu FIPS 180-4 vectors)\n",
           sizeof(vectors) / sizeof(vectors[0]));
    return 0;
}
