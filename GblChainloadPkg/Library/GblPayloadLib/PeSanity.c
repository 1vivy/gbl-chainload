/* GblChainloadPkg/Library/GblPayloadLib/PeSanity.c
   Minimal AArch64 EFI_APPLICATION PE sanity. We do NOT load or relocate;
   we only validate the few fields LoadImage will reject if wrong, plus
   defensive checks the spec calls out.

   This is a host pre-flight: it is compiled into tools/gbl-pack so a
   malformed cached-ABL image is rejected at pack time. It is NOT built
   into the EFI shim (see GblPayloadLib.inf) — at boot, gBS->LoadImage
   is the authority on PE validity. */
#include "Internal/PeSanity.h"

#define DOS_E_LFANEW         0x3C
#define COFF_MACHINE_OFF     0x00
#define COFF_OPT_HDR_SIZE    0x10
#define OPT_MAGIC_OFF        0x00  /* relative to OptionalHeader start */
#define OPT_ENTRY_POINT_OFF  0x10
#define OPT_SIZE_OF_IMAGE    0x38  /* PE32+ SizeOfImage */
#define OPT_SUBSYSTEM_OFF    0x44  /* PE32+ Subsystem (relative to OptionalHeader) */

#define PE_MAGIC_BYTES       0x00004550u  /* "PE\0\0" */
#define MACHINE_AARCH64      0xAA64u
#define OPT_MAGIC_PE32P      0x020Bu
#define SUBSYSTEM_EFI_APP    10u

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

enum gbl_pe_status gbl_pe_sanity(const uint8_t *pe, size_t size) {
    if (size < 0x200) return GBL_PE_TOO_SMALL;
    if (pe[0] != 'M' || pe[1] != 'Z') return GBL_PE_BAD_DOS;
    uint32_t lfanew = le32(pe + DOS_E_LFANEW);
    if ((size_t)lfanew + 0x18 + COFF_OPT_HDR_SIZE > size) return GBL_PE_BAD_LFANEW;
    if (le32(pe + lfanew) != PE_MAGIC_BYTES) return GBL_PE_BAD_PE_MAGIC;
    const uint8_t *coff = pe + lfanew + 4;
    if (le16(coff + COFF_MACHINE_OFF) != MACHINE_AARCH64)
        return GBL_PE_BAD_MACHINE;
    uint16_t opt_size = le16(coff + 0x10);
    if ((size_t)lfanew + 4 + 0x14 + (size_t)opt_size > size) return GBL_PE_BAD_LFANEW;
    const uint8_t *opt = coff + 0x14;
    if (le16(opt + OPT_MAGIC_OFF) != OPT_MAGIC_PE32P) return GBL_PE_BAD_OPT_MAGIC;
    if (le16(opt + OPT_SUBSYSTEM_OFF) != SUBSYSTEM_EFI_APP)
        return GBL_PE_BAD_SUBSYS;
    uint32_t entry = le32(opt + OPT_ENTRY_POINT_OFF);
    uint32_t soi = le32(opt + OPT_SIZE_OF_IMAGE);
    if (entry == 0 || entry >= soi) return GBL_PE_ENTRY_OUT_OF_BOUNDS;
    return GBL_PE_OK;
}
