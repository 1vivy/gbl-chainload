/* tests/host/helpers/test_efisp_scan.c */
#include <stdio.h>
#include <string.h>
#include "../../../tools/shared/efisp_scan.h"

int main(void) {
    /* UTF-16 LE "efisp" + null = 12 bytes */
    static const uint8_t efisp_utf16[] = {
        0x65,0x00, 0x66,0x00, 0x69,0x00, 0x73,0x00, 0x70,0x00, 0x00,0x00
    };
    uint8_t poisoned[256] = {0};
    memcpy(poisoned + 100, efisp_utf16, sizeof(efisp_utf16));
    if (!gbl_contains_utf16_efisp(poisoned, sizeof(poisoned))) {
        fprintf(stderr, "FAIL: missed efisp at offset 100\n");
        return 1;
    }
    uint8_t clean[256] = {0};
    if (gbl_contains_utf16_efisp(clean, sizeof(clean))) {
        fprintf(stderr, "FAIL: false positive on clean buffer\n");
        return 1;
    }
    printf("PASS: efisp scan\n");
    return 0;
}
