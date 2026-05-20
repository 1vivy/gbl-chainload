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
   typedef UINT64 uint64_t;
   typedef INT32  int32_t;
# endif
# ifndef _SIZE_T
#  define _SIZE_T
   typedef __SIZE_TYPE__ size_t;
# endif
#endif

/* Single-shot digest. */
void gbl_sha256(const uint8_t *buf, size_t len, uint8_t out[32]);

/* Incremental (streaming) API. gbl_sha256_ctx is an opaque context; use
 * gbl_sha256_init / gbl_sha256_update / gbl_sha256_final for large inputs
 * that cannot be held in a single buffer (e.g. streaming block devices). */
typedef struct {
  uint8_t  data[64];
  uint32_t datalen;
  uint64_t bitlen;
  uint32_t state[8];
} gbl_sha256_ctx;

void gbl_sha256_init(gbl_sha256_ctx *ctx);
void gbl_sha256_update(gbl_sha256_ctx *ctx, const uint8_t *data, size_t len);
void gbl_sha256_final(gbl_sha256_ctx *ctx, uint8_t out[32]);

#endif
