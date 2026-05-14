/** @file VerifiedBootHook.c — verbose hook for QCOM_VERIFIEDBOOT_PROTOCOL.

  Wraps the 10 vtable entries of `gEfiQcomVerifiedBootProtocolGuid`:

      VBRwDeviceState     — read/write device-state info
      VBDeviceInit        — init with devinfo {is_unlocked, is_unlock_critical}
      VBSendRot           — *** THE Phase B intercept point ***
                            "Send ROT to Keymaster" — constructs RoT payload
                            and ships it to KM TA
      VBSendMilestone     — milestone notification to TZ
      VBVerifyImage       — image verification (pname, img, img_len → bootstate)
      VBDeviceResetState  — reset
      VBIsDeviceSecure    — query secure flag
      VBGetBootState      — read current bootstate (GREEN/YELLOW/ORANGE/RED)
      VBGetCertFingerPrint — yellow-state cert hash
      VBIsKeymasterEnabled — KM availability probe

  Each wrapper logs ONE structured DEBUG line covering both args and
  results (post-call ordering — same as ScmHook), then passes through
  to the saved original. All 10 share a single reentry guard because
  internal traffic kicked off from our logging path can land on any
  slot via SCM/QSEECOM.

  In mode-1 (GBL_MODE == 1), the device-state protocol slots mutate ABL's
  downstream view only: READ_CONFIG and VBDeviceInit report locked/non-critical-
  locked via Mode1Policy_*, while WRITE_CONFIG and VBDeviceResetState are
  swallowed by UniversalPolicy_* so ABL cannot persist lock-state experiments
  back to RPMB. Other slots remain pass-through so GREEN/KeyMaster state is
  produced by ABL's normal code from the locked inputs.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/GblLog.h>
#include <Library/DeviceInfo.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/EFIVerifiedBoot.h>
#include "HookCommon.h"
#include "Mode1Overlay.h"
#include "UniversalBaseline.h"

STATIC QCOM_VERIFIEDBOOT_PROTOCOL    *gHookedVb               = NULL;
STATIC QCOM_VB_RW_DEVICE_STATE        gOrigVbRwDeviceState    = NULL;
STATIC QCOM_VB_DEVICE_INIT            gOrigVbDeviceInit       = NULL;
STATIC QCOM_VB_SEND_ROT               gOrigVbSendRot          = NULL;
STATIC QCOM_VB_SEND_MILESTONE         gOrigVbSendMilestone    = NULL;
STATIC QCOM_VB_VERIFY_IMAGE           gOrigVbVerifyImage      = NULL;
STATIC QCOM_VB_RESET_STATE            gOrigVbResetState       = NULL;
STATIC QCOM_VB_IS_DEVICE_SECURE       gOrigVbIsDeviceSecure   = NULL;
STATIC QCOM_VB_GET_BOOT_STATE         gOrigVbGetBootState     = NULL;
STATIC QCOM_VB_GET_CERT_FINGERPRINT   gOrigVbGetCert          = NULL;
STATIC QCOM_VB_IS_KEYMASTER_ENABLED   gOrigVbIsKmEnabled      = NULL;

HOOK_REENTRY_DEFINE (gVbGuard);

STATIC CONST CHAR8 *
VbBootStateName (
  IN UINT32  S
  )
{
  switch (S) {
    case GREEN:  return "GREEN";
    case ORANGE: return "ORANGE";
    case YELLOW: return "YELLOW";
    case RED:    return "RED";
    default:     return "?";
  }
}

STATIC CONST CHAR8 *
VbDeviceStateOpName (
  IN UINT32  Op
  )
{
  switch (Op) {
    case READ_CONFIG:  return "READ";
    case WRITE_CONFIG: return "WRITE";
    default:           return "?";
  }
}


/* Local hex helper — mirrors QseecomHook's HexN to keep this lib
 * self-contained until we factor out a shared utility header. */
STATIC VOID
VbHex16 (
  IN  CONST UINT8 *Buf,
  IN  UINTN        Len,
  OUT CHAR8       *Out,
  IN  UINTN        OutSize
  )
{
  STATIC CONST CHAR8 kHex[] = "0123456789abcdef";
  UINTN Take, i, j;

  if (Out == NULL || OutSize == 0) return;
  Out[0] = '\0';
  if (Buf == NULL) { AsciiStrCpyS (Out, OutSize, "(null)"); return; }
  if (Len == 0)    { AsciiStrCpyS (Out, OutSize, "(empty)"); return; }

  Take = (Len < 16) ? Len : 16;
  if (OutSize < (Take * 2 + 1)) Take = (OutSize - 1) / 2;
  for (i = 0, j = 0; i < Take; i++) {
    Out[j++] = kHex[(Buf[i] >> 4) & 0xF];
    Out[j++] = kHex[ Buf[i]       & 0xF];
  }
  Out[j] = '\0';
}

/* ---------------- wrappers ---------------- */

STATIC EFI_STATUS EFIAPI
HookedVBRwDeviceState (
  IN     QCOM_VERIFIEDBOOT_PROTOCOL *This,
  IN     vb_device_state_op_t        Op,
  IN OUT UINT8                      *Buf,
  IN     UINT32                      BufLen
  )
{
  EFI_STATUS Status;
  BOOLEAN    First;
  CHAR8      Hex[33];

  First = HookEnter (&gVbGuard);
  if (gOrigVbRwDeviceState == NULL) {
    HookLeave (&gVbGuard);
    return EFI_NOT_READY;
  }
  if (!First) {
    Status = gOrigVbRwDeviceState (This, Op, Buf, BufLen);
#if (GBL_MODE == 1)
    /* Fakelock policy enforced on reentry too — same as first-entry path. */
    if (Op == READ_CONFIG) {
      Mode1Policy_OnVbReadConfig_Post (Status, Buf, BufLen);
    }
#endif
    HookLeave (&gVbGuard);
    return Status;
  }

  /* For WRITE_CONFIG the interesting bytes are the input — snapshot
   * before the call lest the original mutate or zero them. For
   * READ_CONFIG the post-call buffer is what we want. */
  if (Op == WRITE_CONFIG) {
    /* Universal policy: swallow WRITE_CONFIG without forwarding. */
    Status = UniversalPolicy_OnVbWriteConfig ((UINT32)Op, Buf, BufLen);
    HookLeave (&gVbGuard);
    return Status;
  }

  Status = gOrigVbRwDeviceState (This, Op, Buf, BufLen);

#if (GBL_MODE == 1)
  if (Op == READ_CONFIG) {
    Mode1Policy_OnVbReadConfig_Post (Status, Buf, BufLen);
  }
#endif

  if (Op != WRITE_CONFIG) {
    VbHex16 ((CONST UINT8 *)Buf, (UINTN)BufLen, Hex, sizeof (Hex));
  }
  if (First) {
    GBL_INFO ("vb-rwstate | op=%a | bufLen=%u | st=%r\n",
              VbDeviceStateOpName ((UINT32)Op), BufLen, Status);
    VERBOSE ("vb-rwstate | first16=%a\n", Hex);
  }

  HookLeave (&gVbGuard);
  return Status;
}

STATIC EFI_STATUS EFIAPI
HookedVBDeviceInit (
  IN QCOM_VERIFIEDBOOT_PROTOCOL *This,
  IN device_info_vb_t            *Devinfo
  )
{
  EFI_STATUS Status;
  BOOLEAN    First;
  UINT32     Unlocked;
  UINT32     UnlockCritical;

  First = HookEnter (&gVbGuard);
  if (gOrigVbDeviceInit == NULL) {
    HookLeave (&gVbGuard);
    return EFI_NOT_READY;
  }
  if (!First) {
#if (GBL_MODE == 1)
    /* Fakelock policy enforced on reentry too — same as first-entry path. */
    Mode1Policy_OnVbDeviceInit_PrePost (Devinfo, /*IsPre=*/TRUE);
#endif
    Status = gOrigVbDeviceInit (This, Devinfo);
#if (GBL_MODE == 1)
    Mode1Policy_OnVbDeviceInit_PrePost (Devinfo, /*IsPre=*/FALSE);
#endif
    HookLeave (&gVbGuard);
    return Status;
  }

#if (GBL_MODE == 1)
  Mode1Policy_OnVbDeviceInit_PrePost (Devinfo, /*IsPre=*/TRUE);
#endif

  Status = gOrigVbDeviceInit (This, Devinfo);

#if (GBL_MODE == 1)
  Mode1Policy_OnVbDeviceInit_PrePost (Devinfo, /*IsPre=*/FALSE);
#endif
  Unlocked       = (Devinfo != NULL) ? (UINT32)Devinfo->is_unlocked        : 0xFF;
  UnlockCritical = (Devinfo != NULL) ? (UINT32)Devinfo->is_unlock_critical : 0xFF;
  if (First) {
    GBL_INFO ("vb-init | unlocked=%u | unlockCritical=%u | st=%r\n",
              Unlocked, UnlockCritical, Status);
  }

  HookLeave (&gVbGuard);
  return Status;
}

STATIC EFI_STATUS EFIAPI
HookedVBSendRot (
  IN QCOM_VERIFIEDBOOT_PROTOCOL *This
  )
{
  EFI_STATUS Status;
  BOOLEAN    First;

  First = HookEnter (&gVbGuard);
  if (gOrigVbSendRot == NULL) {
    HookLeave (&gVbGuard);
    return EFI_NOT_READY;
  }
  if (!First) {
    Status = gOrigVbSendRot (This);
    HookLeave (&gVbGuard);
    return Status;
  }

  Status = gOrigVbSendRot (This);
  /* Phase B target. The args are (This) only — RoT payload is built
   * internally from devinfo + libavb output. The actual mutating hook
   * will replace this wrapper to inject our locked-format RoT *before*
   * calling the original (or skip the original entirely and emit our
   * own SCM payload via the saved KM AppId). */
  GBL_INFO ("vb-send-rot | (Phase B intercept point) | st=%r\n", Status);

  HookLeave (&gVbGuard);
  return Status;
}

STATIC EFI_STATUS EFIAPI
HookedVBSendMilestone (
  IN QCOM_VERIFIEDBOOT_PROTOCOL *This
  )
{
  EFI_STATUS Status;
  BOOLEAN    First;

  First = HookEnter (&gVbGuard);
  if (gOrigVbSendMilestone == NULL) {
    HookLeave (&gVbGuard);
    return EFI_NOT_READY;
  }
  if (!First) {
    Status = gOrigVbSendMilestone (This);
    HookLeave (&gVbGuard);
    return Status;
  }

  Status = gOrigVbSendMilestone (This);
  GBL_INFO ("vb-milestone | st=%r\n", Status);

  HookLeave (&gVbGuard);
  return Status;
}

STATIC EFI_STATUS EFIAPI
HookedVBVerifyImage (
  IN  QCOM_VERIFIEDBOOT_PROTOCOL *This,
  IN  UINT8                       Pname[MAX_PNAME_LENGTH],
  IN  UINT8                      *Img,
  IN  UINT32                      ImgLen,
  OUT boot_state_t               *BootState
  )
{
  EFI_STATUS  Status;
  BOOLEAN     First;
  CHAR8       PnameSafe[MAX_PNAME_LENGTH + 1];
  UINT32      BS;

  First = HookEnter (&gVbGuard);
  if (gOrigVbVerifyImage == NULL) {
    HookLeave (&gVbGuard);
    return EFI_NOT_READY;
  }
  if (!First) {
    Status = gOrigVbVerifyImage (This, Pname, Img, ImgLen, BootState);
    HookLeave (&gVbGuard);
    return Status;
  }

  Status = gOrigVbVerifyImage (This, Pname, Img, ImgLen, BootState);

  /* pname is a fixed-size byte array; null-terminate defensively before
   * logging as %a. */
  CopyMem (PnameSafe, Pname, MAX_PNAME_LENGTH);
  PnameSafe[MAX_PNAME_LENGTH] = '\0';
  BS = (BootState != NULL) ? (UINT32)*BootState : 0xFFFFFFFFu;

  GBL_INFO ("vb-verify | pname=\"%a\" | imgLen=%u | bootstate=%a(%u) | st=%r\n",
            PnameSafe, ImgLen, VbBootStateName (BS), BS, Status);

  HookLeave (&gVbGuard);
  return Status;
}

STATIC EFI_STATUS EFIAPI
HookedVBResetState (
  IN QCOM_VERIFIEDBOOT_PROTOCOL *This
  )
{
  EFI_STATUS Status;
  BOOLEAN    First;

  First = HookEnter (&gVbGuard);
  if (gOrigVbResetState == NULL) {
    HookLeave (&gVbGuard);
    return EFI_NOT_READY;
  }
  if (!First) {
    /* Universal policy: swallow reset on reentry too — suppression is
     * policy, not just logging. */
    Status = UniversalPolicy_OnVbReset ();
    HookLeave (&gVbGuard);
    return Status;
  }

  /* Universal policy: swallow VBDeviceResetState without forwarding. */
  Status = UniversalPolicy_OnVbReset ();
  HookLeave (&gVbGuard);
  return Status;
}

STATIC EFI_STATUS EFIAPI
HookedVBIsDeviceSecure (
  IN  QCOM_VERIFIEDBOOT_PROTOCOL *This,
  OUT BOOLEAN                    *State
  )
{
  EFI_STATUS Status;
  BOOLEAN    First;
  UINT32     Out;

  First = HookEnter (&gVbGuard);
  if (gOrigVbIsDeviceSecure == NULL) {
    HookLeave (&gVbGuard);
    return EFI_NOT_READY;
  }
  if (!First) {
    Status = gOrigVbIsDeviceSecure (This, State);
    HookLeave (&gVbGuard);
    return Status;
  }

  Status = gOrigVbIsDeviceSecure (This, State);
  Out = (State != NULL) ? (UINT32)*State : 0xFF;
  GBL_INFO ("vb-secure | state=%u | st=%r\n", Out, Status);

  HookLeave (&gVbGuard);
  return Status;
}

STATIC EFI_STATUS EFIAPI
HookedVBGetBootState (
  IN  QCOM_VERIFIEDBOOT_PROTOCOL *This,
  OUT boot_state_t                *BootState
  )
{
  EFI_STATUS Status;
  BOOLEAN    First;
  UINT32     BS;

  First = HookEnter (&gVbGuard);
  if (gOrigVbGetBootState == NULL) {
    HookLeave (&gVbGuard);
    return EFI_NOT_READY;
  }
  if (!First) {
    Status = gOrigVbGetBootState (This, BootState);
    HookLeave (&gVbGuard);
    return Status;
  }

  Status = gOrigVbGetBootState (This, BootState);
  BS = (BootState != NULL) ? (UINT32)*BootState : 0xFFFFFFFFu;
  GBL_INFO ("vb-getstate | bootstate=%a(%u) | st=%r\n",
            VbBootStateName (BS), BS, Status);

  HookLeave (&gVbGuard);
  return Status;
}

STATIC EFI_STATUS EFIAPI
HookedVBGetCertFingerPrint (
  IN  QCOM_VERIFIEDBOOT_PROTOCOL *This,
  OUT UINT8                      *Buf,
  IN  UINTN                       BufLen,
  OUT UINTN                      *OutLen
  )
{
  EFI_STATUS Status;
  BOOLEAN    First;
  UINTN      OL;

  First = HookEnter (&gVbGuard);
  if (gOrigVbGetCert == NULL) {
    HookLeave (&gVbGuard);
    return EFI_NOT_READY;
  }
  if (!First) {
    Status = gOrigVbGetCert (This, Buf, BufLen, OutLen);
    HookLeave (&gVbGuard);
    return Status;
  }

  Status = gOrigVbGetCert (This, Buf, BufLen, OutLen);
  OL = (OutLen != NULL) ? *OutLen : 0;
  GBL_INFO ("vb-cert | bufLen=%lu | outLen=%lu | st=%r\n",
            (UINT64)BufLen, (UINT64)OL, Status);

  HookLeave (&gVbGuard);
  return Status;
}

STATIC EFI_STATUS EFIAPI
HookedVBIsKeymasterEnabled (
  IN  QCOM_VERIFIEDBOOT_PROTOCOL *This,
  OUT BOOLEAN                    *KmEnabled
  )
{
  EFI_STATUS Status;
  BOOLEAN    First;
  UINT32     Out;

  First = HookEnter (&gVbGuard);
  if (gOrigVbIsKmEnabled == NULL) {
    HookLeave (&gVbGuard);
    return EFI_NOT_READY;
  }
  if (!First) {
    Status = gOrigVbIsKmEnabled (This, KmEnabled);
    HookLeave (&gVbGuard);
    return Status;
  }

  Status = gOrigVbIsKmEnabled (This, KmEnabled);
  Out = (KmEnabled != NULL) ? (UINT32)*KmEnabled : 0xFF;
  GBL_INFO ("vb-keymaster-enabled | enabled=%u | st=%r\n", Out, Status);

  HookLeave (&gVbGuard);
  return Status;
}

/* ---------------- installer ---------------- */

EFI_STATUS
InstallVerifiedBootHook (VOID)
{
  EFI_STATUS                  Status;
  QCOM_VERIFIEDBOOT_PROTOCOL *Vb = NULL;
  UINTN                       Installed = 0;
#if (GBL_MODE == 1)
  BOOLEAN                     HaveRwDeviceState = FALSE;
  BOOLEAN                     HaveDeviceInit    = FALSE;
  BOOLEAN                     HaveResetState    = FALSE;
#endif

  if (gHookedVb != NULL) {
    return EFI_ALREADY_STARTED;
  }

  Status = gBS->LocateProtocol (&gEfiQcomVerifiedBootProtocolGuid, NULL,
                                (VOID **)&Vb);
  if (EFI_ERROR (Status) || Vb == NULL) {
    Print (L"VerifiedBootHook: LocateProtocol failed: %r\n", Status);
    return Status;
  }

  /* Per-slot null check + swap. Mirror ScmHook's pattern. */

  if (Vb->VBRwDeviceState != NULL) {
    gOrigVbRwDeviceState = Vb->VBRwDeviceState;
    Vb->VBRwDeviceState  = HookedVBRwDeviceState;
    Installed++;
#if (GBL_MODE == 1)
    HaveRwDeviceState = TRUE;
#endif
  } else { Print (L"VerifiedBootHook: VBRwDeviceState NULL — skip\n"); }

  if (Vb->VBDeviceInit != NULL) {
    gOrigVbDeviceInit = Vb->VBDeviceInit;
    Vb->VBDeviceInit  = HookedVBDeviceInit;
    Installed++;
#if (GBL_MODE == 1)
    HaveDeviceInit = TRUE;
#endif
  } else { Print (L"VerifiedBootHook: VBDeviceInit NULL — skip\n"); }

  if (Vb->VBSendRot != NULL) {
    gOrigVbSendRot = Vb->VBSendRot;
    Vb->VBSendRot  = HookedVBSendRot;
    Installed++;
  } else { Print (L"VerifiedBootHook: VBSendRot NULL — skip\n"); }

  if (Vb->VBSendMilestone != NULL) {
    gOrigVbSendMilestone = Vb->VBSendMilestone;
    Vb->VBSendMilestone  = HookedVBSendMilestone;
    Installed++;
  } else { Print (L"VerifiedBootHook: VBSendMilestone NULL — skip\n"); }

  if (Vb->VBVerifyImage != NULL) {
    gOrigVbVerifyImage = Vb->VBVerifyImage;
    Vb->VBVerifyImage  = HookedVBVerifyImage;
    Installed++;
  } else { Print (L"VerifiedBootHook: VBVerifyImage NULL — skip\n"); }

  if (Vb->VBDeviceResetState != NULL) {
    gOrigVbResetState        = Vb->VBDeviceResetState;
    Vb->VBDeviceResetState   = HookedVBResetState;
    Installed++;
#if (GBL_MODE == 1)
    HaveResetState = TRUE;
#endif
  } else { Print (L"VerifiedBootHook: VBDeviceResetState NULL — skip\n"); }

  if (Vb->VBIsDeviceSecure != NULL) {
    gOrigVbIsDeviceSecure = Vb->VBIsDeviceSecure;
    Vb->VBIsDeviceSecure  = HookedVBIsDeviceSecure;
    Installed++;
  } else { Print (L"VerifiedBootHook: VBIsDeviceSecure NULL — skip\n"); }

  if (Vb->VBGetBootState != NULL) {
    gOrigVbGetBootState = Vb->VBGetBootState;
    Vb->VBGetBootState  = HookedVBGetBootState;
    Installed++;
  } else { Print (L"VerifiedBootHook: VBGetBootState NULL — skip\n"); }

  if (Vb->VBGetCertFingerPrint != NULL) {
    gOrigVbGetCert            = Vb->VBGetCertFingerPrint;
    Vb->VBGetCertFingerPrint  = HookedVBGetCertFingerPrint;
    Installed++;
  } else { Print (L"VerifiedBootHook: VBGetCertFingerPrint NULL — skip\n"); }

  if (Vb->VBIsKeymasterEnabled != NULL) {
    gOrigVbIsKmEnabled         = Vb->VBIsKeymasterEnabled;
    Vb->VBIsKeymasterEnabled   = HookedVBIsKeymasterEnabled;
    Installed++;
  } else { Print (L"VerifiedBootHook: VBIsKeymasterEnabled NULL — skip\n"); }

#if (GBL_MODE == 1)
  if (!HaveRwDeviceState || !HaveDeviceInit || !HaveResetState) {
    Print (L"VerifiedBootHook: mode-1 required slots missing "
           L"(rw=%u init=%u reset=%u)\n",
           (UINT32)HaveRwDeviceState, (UINT32)HaveDeviceInit,
           (UINT32)HaveResetState);
    return EFI_NOT_READY;
  }
#endif

  gHookedVb = Vb;
  GBL_INFO ("VerifiedBootHook: installed %u of 10 slots\n", (UINT32)Installed);
  return EFI_SUCCESS;
}
