#ifndef DPL_PATCH_DESC_H_
#define DPL_PATCH_DESC_H_

#include "ScanLib.h"   /* UINT8/UINT32/BOOLEAN/CHAR8 type aliases incl. host shim. */

typedef enum {
  PATCH_OK         = 0,
  PATCH_MISS       = 1,
  PATCH_AMBIGUOUS  = 2,
} PATCH_OUTCOME;

typedef enum {
  SCOPE_UNIVERSAL     = 0,
  SCOPE_OEM_ONEPLUS   = 1,
  SCOPE_MODE_1        = 2,
  /* SCOPE_MODE_2 and SCOPE_OEM_<other> can be added by later plans. */
} PATCH_SCOPE;

typedef PATCH_OUTCOME (*PATCH_APPLY)(UINT8 *Buf, UINT32 Size);

typedef struct {
  CONST CHAR8  *Name;
  PATCH_SCOPE   Scope;
  BOOLEAN       Mandatory;
  PATCH_APPLY   Apply;
} PATCH_DESC;

#endif /* DPL_PATCH_DESC_H_ */
