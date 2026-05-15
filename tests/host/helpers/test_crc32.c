/* tests/host/helpers/test_crc32.c */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../../GblChainloadPkg/Library/GblPayloadLib/Internal/Crc32.h"

int main(void) {
    /* IEEE 802.3 CRC-32 of "123456789" is 0xCBF43926. */
    static const uint8_t v[] = "123456789";
    uint32_t got = gbl_crc32(v, 9);
    if (got != 0xCBF43926u) {
        fprintf(stderr, "FAIL: crc32 expected 0xcbf43926, got 0x%08x\n", got);
        return 1;
    }
    printf("PASS: crc32\n");
    return 0;
}
