/** @file ProfileOverlay.h — profile-spoof hook policy declarations.

    Declarations are unconditionally available; activation is runtime-gated
    at every call site on gManifest.WantProfileSpoof (BootFlow loads the
    profile; QseecomHook / SpssHook take the gate inline before rewrite). **/
#ifndef PROFILE_OVERLAY_H_
#define PROFILE_OVERLAY_H_

#include <Uefi.h>

#include "../../../crates/mode2-profile-core/include/mode2_profile_ffi.h"

/* Store a validated profile. Copies *Profile into module state and
   sets the internal gMode2HasProfile flag. Called once by BootFlow. */
VOID EFIAPI ProfileOverlay_SetProfile (IN CONST struct gbl_mode2_profile *Profile);

/* QseecomSendCmd policy: rewrite a KM send buffer in place from the
   stored profile. No-op (returns FALSE) if no profile is stored or the
   cmd-id is not a spoof target. Emits a GBL_INFO line on a rewrite. */
BOOLEAN EFIAPI
ProfileOverlay_RewriteKmSend (IN     UINT32  CmdId,
                              IN OUT UINT8  *SendBuf,
                              IN     UINT32  SendLen);

/* SPSS ShareKeyMintInfo policy: rewrite the packed RoT/BootState/Vbh
   struct in place from the stored profile. No-op if no profile. */
BOOLEAN EFIAPI
ProfileOverlay_RewriteSpss (IN OUT VOID   *Info,
                            IN     UINT32  InfoLen);

#endif /* PROFILE_OVERLAY_H_ */
