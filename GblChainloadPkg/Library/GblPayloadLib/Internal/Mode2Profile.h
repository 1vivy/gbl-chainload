#ifndef GBL_MODE2_PROFILE_PARSE_H_
#define GBL_MODE2_PROFILE_PARSE_H_

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

#include "../../../../tools/shared/gbl_mode2_profile.h"

enum gbl_m2p_status {
    GBL_M2P_OK = 0,
    GBL_M2P_TOO_SMALL,
    GBL_M2P_BAD_MAGIC,
    GBL_M2P_BAD_VERSION,
    GBL_M2P_BAD_RESERVED,
    GBL_M2P_BAD_FIELD       /* is_unlocked > 1 or color > 3 */
};

/* Parse and validate a mode2_profile payload. `bytes` is the 0x0010
   entry payload, `size` its byte length. On GBL_M2P_OK, *out is filled
   with host-endian field values. */
enum gbl_m2p_status
gbl_mode2_profile_parse(const uint8_t *bytes, size_t size,
                        struct gbl_mode2_profile *out);

#endif
