#ifndef GBL_CRC32_H_
#define GBL_CRC32_H_

#ifdef GBL_HOST_BUILD
# include <stdint.h>
# include <stddef.h>
#else
# include <Uefi.h>
  typedef UINT8  uint8_t;
  typedef UINT32 uint32_t;
  typedef UINTN  size_t;
#endif

uint32_t gbl_crc32(const uint8_t *buf, size_t len);

#endif
