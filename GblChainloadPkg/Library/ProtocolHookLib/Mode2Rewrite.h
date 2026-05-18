/* GblChainloadPkg/Library/ProtocolHookLib/Mode2Rewrite.h — pure-logic
   KM/SPSS buffer rewrite. No EDK2 dependency; host-testable. */
#ifndef GBL_MODE2_REWRITE_H_
#define GBL_MODE2_REWRITE_H_

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

#include "../../../tools/shared/gbl_mode2_profile.h"

/* KeyMaster wire sizes (KEYMASTER_UTILS_CMD_ID = 0x200 + N). */
#define GBL_KM_CMD_SET_ROT          0x00000201u
#define GBL_KM_CMD_SET_VERSION      0x00000207u
#define GBL_KM_CMD_SET_BOOT_STATE   0x00000208u
#define GBL_KM_CMD_SET_VBH          0x00000211u
#define GBL_KM_LEN_SET_ROT          44u
#define GBL_KM_LEN_SET_VERSION      12u
#define GBL_KM_LEN_SET_BOOT_STATE   64u
#define GBL_KM_LEN_SET_VBH          36u

/* Rewrite a KM send buffer in place from `p`. `cmd_id` is the leading
   u32 of the buffer; `buf`/`len` the send buffer. Rewrites only the
   four spoof-target cmd-ids and only when `len` matches the wire size.
   Returns 1 if a rewrite happened, 0 otherwise. Safe on NULL/short buf. */
int gbl_m2_rewrite_km(uint32_t cmd_id, uint8_t *buf, uint32_t len,
                      const struct gbl_mode2_profile *p);

/* SPSS ShareKeyMintInfo carries a packed
   { KmSetRotReqWire(44), KmSetBootStateReqWire(64), KmSetVbhReqWire(36) }.
   Rewrite all three sub-structs in place. `info`/`info_len` is the whole
   packed struct (>= 144 bytes). Returns 1 if rewritten, 0 otherwise. */
#define GBL_SPSS_INFO_LEN  144u
int gbl_m2_rewrite_spss(uint8_t *info, uint32_t info_len,
                        const struct gbl_mode2_profile *p);

#endif
