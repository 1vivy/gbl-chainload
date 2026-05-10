#ifndef AVB_BIG_ENDIAN_H_
#define AVB_BIG_ENDIAN_H_

#ifdef __HOST_BUILD__
#include <stdint.h>
#include <stddef.h>
#ifndef UINT8
typedef uint8_t  UINT8;
#endif
#ifndef UINT32
typedef uint32_t UINT32;
#endif
#ifndef UINT64
typedef uint64_t UINT64;
#endif
#ifndef CHAR8
typedef char CHAR8;
#endif
#ifndef EFIAPI
#define EFIAPI
#endif
#ifndef IN
#define IN
#endif
#ifndef OUT
#define OUT
#endif
#ifndef STATIC
#define STATIC static
#endif
#ifndef CONST
#define CONST const
#endif
typedef int EFI_STATUS;
#define EFI_SUCCESS              0
#define EFI_NOT_FOUND            14
#define EFI_INVALID_PARAMETER    2
#define EFI_END_OF_MEDIA         28
#else
#include <Uefi.h>
#endif

STATIC UINT32 AvbReadU32Be (CONST UINT8 *Buf) {
  return ((UINT32)Buf[0] << 24) | ((UINT32)Buf[1] << 16)
       | ((UINT32)Buf[2] << 8)  |  (UINT32)Buf[3];
}

STATIC UINT64 AvbReadU64Be (CONST UINT8 *Buf) {
  return ((UINT64)Buf[0] << 56) | ((UINT64)Buf[1] << 48)
       | ((UINT64)Buf[2] << 40) | ((UINT64)Buf[3] << 32)
       | ((UINT64)Buf[4] << 24) | ((UINT64)Buf[5] << 16)
       | ((UINT64)Buf[6] << 8)  |  (UINT64)Buf[7];
}

#endif
