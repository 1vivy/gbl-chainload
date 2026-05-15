#ifndef GBL_SHA256_H_
#define GBL_SHA256_H_

#ifdef GBL_HOST_BUILD
# include <stdint.h>
# include <stddef.h>
#else
# include <Uefi.h>
  typedef UINT8  uint8_t;
  typedef UINTN  size_t;
#endif

void gbl_sha256(const uint8_t *buf, size_t len, uint8_t out[32]);

#endif
