/* lzma_compat.h — thin shim: lzma_alone_decompress() via Igor Pavlov's LzmaDec.
   Used for the Android cross-compile target of fv-unwrap where liblzma is not
   available.  The .lzma (LZMA alone) format header is:
     [0..4]   5 bytes props
     [5..12]  8 bytes uncompressed size (LE, UINT64_MAX = unknown)
   Compressed payload starts at byte 13. */
#ifndef LZMA_COMPAT_H_
#define LZMA_COMPAT_H_

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "LzmaDec.h"

/* ISzAlloc implementation using malloc/free */
static void *_lzma_compat_alloc(ISzAllocPtr p, size_t sz) { (void)p; return malloc(sz); }
static void  _lzma_compat_free (ISzAllocPtr p, void *a)   { (void)p; free(a); }
static const ISzAlloc _lzma_compat_allocator = {
  _lzma_compat_alloc, _lzma_compat_free
};

/* Drop-in replacement for liblzma's lzma_alone_decompress().
   Returns heap buffer of *outSz bytes, or NULL on failure.
   Caller must free() the result. */
static inline uint8_t *lzma_alone_decompress(const uint8_t *in, size_t inSz,
                                              size_t *outSz)
{
  if (inSz < 13) return NULL;

  /* Parse uncompressed size from bytes [5..12] LE */
  uint64_t unc64;
  memcpy(&unc64, in + 5, 8);
  size_t alloc = (unc64 == UINT64_MAX || unc64 > 256*1024*1024)
                 ? 256*1024*1024 : (size_t)unc64;

  uint8_t *out = (uint8_t *)malloc(alloc);
  if (!out) return NULL;

  /* props are the first 5 bytes; payload starts at byte 13 */
  SizeT destLen = (SizeT)alloc;
  SizeT srcLen  = (SizeT)(inSz - 13);
  ELzmaStatus status;
  SRes res = LzmaDecode(out, &destLen,
                        in + 13, &srcLen,
                        in,      LZMA_PROPS_SIZE,
                        LZMA_FINISH_ANY, &status,
                        &_lzma_compat_allocator);

  *outSz = (size_t)destLen;

  if (res != SZ_OK && *outSz == 0) {
    free(out);
    return NULL;
  }
  /* Mirror liblzma's lenient behaviour: return partial output on soft errors */
  return out;
}

#endif /* LZMA_COMPAT_H_ */
