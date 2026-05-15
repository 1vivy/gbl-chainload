#ifndef GBL_SHA256_H_
#define GBL_SHA256_H_

#ifdef GBL_HOST_BUILD
# include <stdint.h>
# include <stddef.h>
#else
# include <Uefi.h>
# ifndef GBL_COMPAT_TYPES_DEFINED
#  define GBL_COMPAT_TYPES_DEFINED
   typedef UINT8  uint8_t;
   typedef UINT16 uint16_t;
   typedef UINT32 uint32_t;
   typedef INT32  int32_t;
# endif
# ifndef _SIZE_T
#  define _SIZE_T
   typedef __SIZE_TYPE__ size_t;
# endif
#endif

void gbl_sha256(const uint8_t *buf, size_t len, uint8_t out[32]);

#endif
