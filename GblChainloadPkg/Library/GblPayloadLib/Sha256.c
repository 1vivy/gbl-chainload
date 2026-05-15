/* GblChainloadPkg/Library/GblPayloadLib/Sha256.c
   Host: libcrypto. EDK2: OpenSslLib's SHA256_*. */
#include "Internal/Sha256.h"

#ifdef GBL_HOST_BUILD
# include <openssl/sha.h>
void gbl_sha256(const uint8_t *buf, size_t len, uint8_t out[32]) {
    SHA256_CTX c;
    SHA256_Init(&c);
    SHA256_Update(&c, buf, len);
    SHA256_Final(out, &c);
}
#else
# include <Library/BaseCryptLib.h>
void gbl_sha256(const uint8_t *buf, size_t len, uint8_t out[32]) {
    Sha256HashAll(buf, len, out);
}
#endif
