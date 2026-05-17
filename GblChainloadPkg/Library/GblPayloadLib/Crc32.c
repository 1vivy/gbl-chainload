/* GblChainloadPkg/Library/GblPayloadLib/Crc32.c
   IEEE 802.3 CRC-32 — reflected polynomial 0xEDB88320, initial and
   final-XOR value 0xFFFFFFFF. This is bit-identical to zlib's crc32()
   and to EDK2's BaseLib CalculateCrc32 / gBS->CalculateCrc32.

   Vendored deliberately, for the same reason as Sha256.c: one source is
   compiled into the EDK2 shim and both host/Android `gbl-pack` builds,
   so the GBLP1 header CRC is computed by identical code on producer and
   consumer. gBS->CalculateCrc32 would save 14 lines but adds a
   boot-service dependency the shim avoids across handoff, and would not
   cover the host packer at all. See Sha256.c for the full rationale.

   Pinned in CI by a known-answer vector (CRC-32("123456789") ==
   0xCBF43926): tests/host/helpers/test_crc32.c, run via
   tests/host/070_crypto_conformance.sh. */
#include "Internal/Crc32.h"

uint32_t gbl_crc32(const uint8_t *buf, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}
