/* tools/shared/efisp_scan.h — UTF-16 LE "efisp" byte-scan helper.
   Shared between EDK2 DynamicPatchLib and host tools/gbl-pack. */
#ifndef GBL_EFISP_SCAN_H_
#define GBL_EFISP_SCAN_H_

#include <stdint.h>
#include <stddef.h>

static inline int
gbl_contains_utf16_efisp(const uint8_t *buf, size_t len) {
    /* "efisp" in UTF-16 LE plus a trailing null wide char = 12 bytes. */
    static const uint8_t pat[12] = {
        0x65,0x00, 0x66,0x00, 0x69,0x00, 0x73,0x00, 0x70,0x00, 0x00,0x00
    };
    if (len < sizeof(pat)) return 0;
    for (size_t i = 0; i + sizeof(pat) <= len; i++) {
        if (buf[i] == pat[0] &&
            !__builtin_memcmp(buf + i, pat, sizeof(pat))) {
            return 1;
        }
    }
    return 0;
}

#endif /* GBL_EFISP_SCAN_H_ */
