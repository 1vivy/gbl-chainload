/** @file Mode2Overlay.c — mode-2-scope hook policy implementation.
    Holds the validated profile and applies the QSEE/SPSS rewrites.
    Compiled out entirely in non-mode-2 builds via the GBL_MODE guard. **/
#include "Mode2Overlay.h"

#if (GBL_MODE == 2)

#include <Library/BaseMemoryLib.h>
#include <Library/GblLog.h>
#include "Mode2Rewrite.h"

STATIC struct gbl_mode2_profile  gMode2Profile;
STATIC BOOLEAN                   gMode2HasProfile = FALSE;

VOID EFIAPI
Mode2_SetProfile (IN CONST struct gbl_mode2_profile *Profile) {
  if (Profile == NULL) return;
  CopyMem (&gMode2Profile, Profile, sizeof (gMode2Profile));
  gMode2HasProfile = TRUE;
  GBL_INFO ("mode2 | profile set (ver=%u color=%u isUnlocked=%u)\n",
            (UINT32)Profile->version, Profile->color, Profile->is_unlocked);
}

BOOLEAN EFIAPI
Mode2_HasProfile (VOID) {
  return gMode2HasProfile;
}

BOOLEAN EFIAPI
Mode2Policy_RewriteKmSend (IN UINT32 CmdId, IN OUT UINT8 *SendBuf,
                           IN UINT32 SendLen) {
  if (!gMode2HasProfile) return FALSE;
  if (gbl_m2_rewrite_km (CmdId, SendBuf, SendLen, &gMode2Profile)) {
    GBL_INFO ("mode2 | km-rewrite | cmd=0x%08x | len=%u\n", CmdId, SendLen);
    return TRUE;
  }
  return FALSE;
}

BOOLEAN EFIAPI
Mode2Policy_RewriteSpss (IN OUT VOID *Info, IN UINT32 InfoLen) {
  if (!gMode2HasProfile || Info == NULL) return FALSE;
  if (gbl_m2_rewrite_spss ((UINT8 *)Info, InfoLen, &gMode2Profile)) {
    GBL_INFO ("mode2 | spss-rewrite | len=%u\n", InfoLen);
    return TRUE;
  }
  return FALSE;
}

#endif /* GBL_MODE == 2 */
