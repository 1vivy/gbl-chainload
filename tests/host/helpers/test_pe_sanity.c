/* tests/host/helpers/test_pe_sanity.c */
#include <stdio.h>
#include <string.h>
#include "../../../GblChainloadPkg/Library/GblPayloadLib/Internal/PeSanity.h"

/* Minimal synthetic PE bytes for a sane AArch64 EFI_APPLICATION.
   Real PEs are larger; this exercises the header-parsing path only. */
static uint8_t make_sane_pe[1024];

static void build_sane_pe(void) {
    memset(make_sane_pe, 0, sizeof(make_sane_pe));
    /* DOS magic */
    make_sane_pe[0] = 'M'; make_sane_pe[1] = 'Z';
    /* e_lfanew at 0x3c -> 0x80 */
    make_sane_pe[0x3c] = 0x80;
    /* PE\0\0 at 0x80 */
    make_sane_pe[0x80] = 'P'; make_sane_pe[0x81] = 'E';
    /* COFF header at 0x84 (after PE signature): Machine=0xAA64 at offset 0x00 */
    make_sane_pe[0x84] = 0x64; make_sane_pe[0x85] = 0xAA;
    /* SizeOfOptionalHeader = 0xF0 at offset 0x10 in COFF = 0x94 (PE32+ size) */
    make_sane_pe[0x94] = 0xF0;
    /* Optional header starts at 0x98 (COFF + 0x14) */
    /* Optional header magic 0x020B (PE32+) at offset 0x00 = 0x98 */
    make_sane_pe[0x98] = 0x0B; make_sane_pe[0x99] = 0x02;
    /* AddressOfEntryPoint at offset 0x10 = 0xA8 */
    make_sane_pe[0xA8] = 0x00; make_sane_pe[0xA9] = 0x10;
    /* SizeOfImage at offset 0x38 = 0xD0 */
    make_sane_pe[0xD0] = 0x00; make_sane_pe[0xD1] = 0x00;
    make_sane_pe[0xD2] = 0x01; make_sane_pe[0xD3] = 0x00;
    /* Subsystem at offset 0x44 = 0xDC (EFI_APPLICATION = 10) */
    make_sane_pe[0xDC] = 10;
}

int main(void) {
    build_sane_pe();
    enum gbl_pe_status s;

    s = gbl_pe_sanity(make_sane_pe, sizeof(make_sane_pe));
    if (s != GBL_PE_OK) {
        fprintf(stderr, "FAIL: sane PE rejected: %d\n", s);
        return 1;
    }

    /* Bad MZ */
    make_sane_pe[0] = 'X';
    s = gbl_pe_sanity(make_sane_pe, sizeof(make_sane_pe));
    if (s != GBL_PE_BAD_DOS) { fprintf(stderr, "FAIL: bad MZ\n"); return 1; }
    make_sane_pe[0] = 'M';

    /* Wrong machine */
    make_sane_pe[0x84] = 0x64; make_sane_pe[0x85] = 0x86; /* 0x8664 = x64 */
    s = gbl_pe_sanity(make_sane_pe, sizeof(make_sane_pe));
    if (s != GBL_PE_BAD_MACHINE) { fprintf(stderr, "FAIL: machine\n"); return 1; }
    make_sane_pe[0x84] = 0x64; make_sane_pe[0x85] = 0xAA;

    /* Wrong subsystem */
    make_sane_pe[0xDC] = 3; /* 3 = WINDOWS_CUI */
    s = gbl_pe_sanity(make_sane_pe, sizeof(make_sane_pe));
    if (s != GBL_PE_BAD_SUBSYS) { fprintf(stderr, "FAIL: subsys\n"); return 1; }
    make_sane_pe[0xDC] = 10;

    /* Wraparound: lfanew near UINT32_MAX must be rejected, not bypass bounds. */
    /* Reset to sane PE first */
    build_sane_pe();
    /* e_lfanew = 0xFFFFFFF0 — would wrap if checked with uint32_t arithmetic */
    make_sane_pe[0x3c] = 0xF0; make_sane_pe[0x3d] = 0xFF;
    make_sane_pe[0x3e] = 0xFF; make_sane_pe[0x3f] = 0xFF;
    s = gbl_pe_sanity(make_sane_pe, sizeof(make_sane_pe));
    if (s != GBL_PE_BAD_LFANEW) {
        fprintf(stderr, "FAIL: lfanew wraparound not rejected: %d\n", s);
        return 1;
    }

    printf("PASS: pe_sanity\n");
    return 0;
}
