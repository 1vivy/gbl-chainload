/* tests/host/helpers/test_sha256.c */
#include <stdio.h>
#include <string.h>
#include "../../../GblChainloadPkg/Library/GblPayloadLib/Internal/Sha256.h"

int main(void) {
    /* SHA-256("abc") = ba7816bf 8f01cfea 414140de 5dae2223
                        b00361a3 96177a9c b410ff61 f20015ad */
    static const uint8_t expected[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
        0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
        0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    uint8_t got[32];
    gbl_sha256((const uint8_t *)"abc", 3, got);
    if (memcmp(got, expected, 32) != 0) {
        fprintf(stderr, "FAIL: sha256(abc) mismatch\n");
        return 1;
    }
    printf("PASS: sha256\n");
    return 0;
}
