#ifndef GBL_PE_SANITY_H_
#define GBL_PE_SANITY_H_

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

enum gbl_pe_status {
    GBL_PE_OK = 0,
    GBL_PE_TOO_SMALL,
    GBL_PE_BAD_DOS,
    GBL_PE_BAD_LFANEW,
    GBL_PE_BAD_PE_MAGIC,
    GBL_PE_BAD_MACHINE,
    GBL_PE_BAD_OPT_MAGIC,
    GBL_PE_BAD_SUBSYS,
    GBL_PE_ENTRY_OUT_OF_BOUNDS
};

enum gbl_pe_status gbl_pe_sanity(const uint8_t *pe, size_t size);

#endif
