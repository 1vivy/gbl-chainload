/** @file SpssHook.c — observation-only hook for the SPSS (Secure
  Processor SubSystem) bridge protocol.

  The SPSS bridge mirrors KeyMaster RoT / BootState / Vbh writes to the
  SPU — an enforcement domain independent of TZ-side keymaster.
  Locked-state ground truth lives partly in SPU, so we need visibility
  into what ABL sends to it. SPU traffic is invisible if we only watch
  QSEECOM/SCM.

  The protocol exposes a single vtable slot:
    SPSSDxe_ShareKeyMintInfo (IN KeymintSharedInfoStruct *Info)
  carrying a packed { KMSetRotReq, KMSetBootStateReq, KMSetVbhReq }.

  This file pulls the canonical types from
  edk2/QcomModulePkg/Include/Protocol/EFISPSS.h, so all field offsets
  and the protocol GUID stay in sync with the BSP. Earlier port had
  inline-declared structs + a placeholder GUID — see git history.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/LogFsLib.h>  /* GBL_DBG_LOGFS_ONLY level bit */
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/EFISPSS.h>
#include "HookCommon.h"

STATIC SpssProtocol             *gHookedSpss           = NULL;
STATIC SPSS_SHARE_KEYMINT_INFO   gOrigShareKeyMintInfo = NULL;

HOOK_REENTRY_DEFINE (gSpssGuard);

/** Lowercase-hex up to MaxBytes of Buf into Out. NUL-terminated.
    Out must be >= MaxBytes*2 + 1. **/
STATIC VOID
SpssHexN (
  IN  CONST UINT8 *Buf,
  IN  UINTN        Len,
  IN  UINTN        MaxBytes,
  OUT CHAR8       *Out,
  IN  UINTN        OutSize
  )
{
  STATIC CONST CHAR8 kHex[] = "0123456789abcdef";
  UINTN Take, i, j;

  if (Out == NULL || OutSize == 0) return;
  Out[0] = '\0';
  if (Buf == NULL) { AsciiStrCpyS (Out, OutSize, "(null)");  return; }
  if (Len == 0)    { AsciiStrCpyS (Out, OutSize, "(empty)"); return; }

  Take = (Len < MaxBytes) ? Len : MaxBytes;
  if (OutSize < (Take * 2 + 1)) {
    Take = (OutSize - 1) / 2;
  }
  for (i = 0, j = 0; i < Take; i++) {
    Out[j++] = kHex[(Buf[i] >> 4) & 0xF];
    Out[j++] = kHex[ Buf[i]       & 0xF];
  }
  Out[j] = '\0';
}

STATIC EFI_STATUS EFIAPI
HookedShareKeyMintInfo (
  IN KeymintSharedInfoStruct *Info
  )
{
  EFI_STATUS  Status;
  BOOLEAN     First;

  First = HookEnter (&gSpssGuard);

  if (gOrigShareKeyMintInfo == NULL) {
    HookLeave (&gSpssGuard);
    return EFI_NOT_READY;
  }

  if (!First || Info == NULL) {
    Status = gOrigShareKeyMintInfo (Info);
    HookLeave (&gSpssGuard);
    return Status;
  }

  CHAR8 RotHex[65], PubKeyHex[65], VbhHex[65];

  SpssHexN ((CONST UINT8 *)Info->RootOfTrust.RotDigest,
            sizeof (Info->RootOfTrust.RotDigest),
            32, RotHex, sizeof (RotHex));

  SpssHexN ((CONST UINT8 *)Info->BootInfo.BootState.PublicKey,
            sizeof (Info->BootInfo.BootState.PublicKey),
            32, PubKeyHex, sizeof (PubKeyHex));

  SpssHexN ((CONST UINT8 *)Info->Vbh.Vbh,
            sizeof (Info->Vbh.Vbh),
            32, VbhHex, sizeof (VbhHex));

  DEBUG ((GBL_DBG_LOGFS_ONLY,
          "spss-rot | cmd=0x%x | offset=%u | size=%u | digest=%a\n",
          Info->RootOfTrust.CmdId,
          Info->RootOfTrust.RotOffset,
          Info->RootOfTrust.RotSize,
          RotHex));

  DEBUG ((GBL_DBG_LOGFS_ONLY,
          "spss-bootstate | cmd=0x%x | ver=%u | offset=%u | size=%u | "
          "unlocked=%u | pubKey=%a | color=%u | sysVer=0x%x | sysSpl=0x%x\n",
          Info->BootInfo.CmdId,
          Info->BootInfo.Version,
          Info->BootInfo.Offset,
          Info->BootInfo.Size,
          Info->BootInfo.BootState.IsUnlocked,
          PubKeyHex,
          Info->BootInfo.BootState.Color,
          Info->BootInfo.BootState.SystemVersion,
          Info->BootInfo.BootState.SystemSecurityLevel));

  DEBUG ((GBL_DBG_LOGFS_ONLY,
          "spss-vbh | cmd=0x%x | digest=%a\n",
          Info->Vbh.CmdId, VbhHex));

  Status = gOrigShareKeyMintInfo (Info);

  DEBUG ((GBL_DBG_LOGFS_ONLY, "spss-share | st=%r\n", Status));

  HookLeave (&gSpssGuard);
  return Status;
}

EFI_STATUS
InstallSpssHook (VOID)
{
  EFI_STATUS     Status;
  SpssProtocol  *Spss = NULL;

  if (gHookedSpss != NULL) {
    return EFI_ALREADY_STARTED;
  }

  Status = gBS->LocateProtocol (&gEfiSPSSProtocolGuid, NULL, (VOID **)&Spss);
  if (EFI_ERROR (Status) || Spss == NULL) {
    Print (L"SpssHook: LocateProtocol failed: %r\n", Status);
    return Status;
  }

  if (Spss->SPSSDxe_ShareKeyMintInfo == NULL) {
    Print (L"SpssHook: ShareKeyMintInfo slot is NULL\n");
    return EFI_NOT_READY;
  }

  gOrigShareKeyMintInfo          = Spss->SPSSDxe_ShareKeyMintInfo;
  Spss->SPSSDxe_ShareKeyMintInfo = HookedShareKeyMintInfo;
  gHookedSpss                    = Spss;

  DEBUG ((DEBUG_INFO, "SpssHook: installed ShareKeyMintInfo=%p (orig=%p)\n",
          HookedShareKeyMintInfo, gOrigShareKeyMintInfo));
  return EFI_SUCCESS;
}
