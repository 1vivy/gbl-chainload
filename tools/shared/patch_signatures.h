/* tools/shared/patch_signatures.h — single authoritative source for ABL
   patch signatures shared between EDK2 DynamicPatchLib and host tools.

   EDK2 builds include this via the per-mode Signatures.h wrappers.
   Host builds include it directly or via those same wrappers when compiled
   with -D__HOST_BUILD__ (the project-wide host-build guard). */
#ifndef GBL_PATCH_SIGNATURES_H_
#define GBL_PATCH_SIGNATURES_H_

#if defined(__HOST_BUILD__) || defined(GBL_HOST_BUILD)
# include <stdint.h>
  /* EDK2 annotation macros needed so the arrays below compile on the host. */
# ifndef STATIC
#   define STATIC static
# endif
# ifndef CONST
#   define CONST const
# endif
# ifndef UINT8
  typedef uint8_t UINT8;
# endif
#else
# include <Uefi.h>
#endif

/* The 10-byte UTF-16 LE pattern that ABL searches for when probing EFISP
   for the GBL chainload app.  Patch1 zeroes every occurrence it finds.
   10 bytes = 5 chars × 2 bytes/char UTF-16 LE, no trailing wide-null.
   (Distinct from the 12-byte variant in tools/shared/efisp_scan.h which
   includes the trailing wide null and is used only for host-side detection.) */
STATIC CONST UINT8 kEfispUtf16Pattern[10] = {
  0x65, 0x00,  /* e */
  0x66, 0x00,  /* f */
  0x69, 0x00,  /* i */
  0x73, 0x00,  /* s */
  0x70, 0x00   /* p */
};

/* Add other signatures that are shared between host tools and DynamicPatchLib
   below this line as they are identified. */

#endif /* GBL_PATCH_SIGNATURES_H_ */
