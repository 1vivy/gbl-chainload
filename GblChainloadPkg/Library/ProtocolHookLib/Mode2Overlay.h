/** @file Mode2Overlay.h — mode-2-scope hook policy declarations.
    Only active when GBL_MODE == 2. **/
#ifndef MODE2_OVERLAY_H_
#define MODE2_OVERLAY_H_

#include <Uefi.h>

#if (GBL_MODE == 2)

#include "../../../tools/shared/gbl_mode2_profile.h"

/* Store a validated profile. Copies *Profile into module state and
   sets the internal gMode2HasProfile flag. Called once by BootFlow. */
VOID EFIAPI Mode2_SetProfile (IN CONST struct gbl_mode2_profile *Profile);

/* QseecomSendCmd policy: rewrite a KM send buffer in place from the
   stored profile. No-op (returns FALSE) if no profile is stored or the
   cmd-id is not a spoof target. Emits a GBL_INFO line on a rewrite. */
BOOLEAN EFIAPI
Mode2Policy_RewriteKmSend (IN     UINT32  CmdId,
                           IN OUT UINT8  *SendBuf,
                           IN     UINT32  SendLen);

/* SPSS ShareKeyMintInfo policy: rewrite the packed RoT/BootState/Vbh
   struct in place from the stored profile. No-op if no profile. */
BOOLEAN EFIAPI
Mode2Policy_RewriteSpss (IN OUT VOID   *Info,
                         IN     UINT32  InfoLen);

#endif /* GBL_MODE == 2 */
#endif /* MODE2_OVERLAY_H_ */
