/** @file QseecomHook.c — verbose hook for QCOM_QSEECOM_PROTOCOL.

  We hook two slots on the singleton QSEECOM protocol:

    * QseecomStartApp (vtable slot 0x8) — logs the TA name and the AppId
      assigned. AppIds are issued sequentially in load order, so this is
      the missing link between cmd-traffic `handle` values and TA names.

    * QseecomSendCmd (vtable slot 0x18) — logs cmd_id, handle, send/rsp
      lengths, and a hex prefix of each buffer. Buffer-prefix width is
      keyed on TA: TAs whose AppId we recognise as Phoenix-shaped (4 KB
      page traffic) get 128 B; everything else gets 32 B.

  All wrappers pass through to the original implementation; no request
  or response mutation here. Mutation lives in Phase B/C libs.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/GblLog.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/EFIQseecom.h>
#include "HookCommon.h"
#include "UniversalBaseline.h"

STATIC QCOM_QSEECOM_SEND_CMD_APP gOriginalSendCmd  = NULL;
STATIC QCOM_QSEECOM_START_APP    gOriginalStartApp = NULL;
STATIC QCOM_QSEECOM_PROTOCOL    *gHookedProtocol   = NULL;

HOOK_REENTRY_DEFINE (gQseecomSendGuard);
HOOK_REENTRY_DEFINE (gQseecomStartGuard);

/* Track the AppId of the most-recently-started Phoenix-shaped TA. Updated
 * on every QseecomStartApp; QseecomSendCmd uses it to widen the dump on
 * Phoenix traffic. -1U means "no Phoenix TA known yet". */
STATIC UINT32 gPhoenixHandle  = (UINT32)-1;
STATIC UINT32 gOplusSecHandle = (UINT32)-1;

/* OplusSec TA is identified at QseecomStartApp by 16-byte EFI_GUID rather
 * than ASCII name (Ghidra G1 on LinuxLoader_infiniti.efi mem:0007f1e0).
 * GUID literal: {E11DDA6A-651B-4AB4-B8C5-30B352B472E2} → little-endian
 * canonical EFI_GUID byte layout. */
STATIC CONST UINT8 kOplusSecGuidBytes[16] = {
  0x6A, 0xDA, 0x1D, 0xE1, 0x1B, 0x65, 0xB4, 0x4A,
  0xB8, 0xC5, 0x30, 0xB3, 0x52, 0xB4, 0x72, 0xE2,
};

#define QSEE_APPNAME_MAX  64u

/** Format up-to-MaxBytes bytes of Buf as lowercase hex into Out.
    Out must be >= MaxBytes*2 + 1. Always NUL-terminates. **/
STATIC VOID
HexN (
  IN  CONST UINT8 *Buf,
  IN  UINT32       Len,
  IN  UINTN        MaxBytes,
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

/* DEBUG()'s underlying format buffer is 256 chars on EDK2's stock UefiDebugLib,
 * so any single line over ~200 hex chars + meta gets silently truncated. For
 * wide dumps we therefore emit continuation lines, each bounded by
 * QSEE_DUMP_CHUNK_BYTES so 2*chunk + meta stays well under 256.
 *
 * Format:  qsee-buf | h=<handle> | dir=<s|r> | off=<offset> | hex=<...>
 */
#define QSEE_DUMP_CHUNK_BYTES   64u

STATIC VOID
DumpChunked (
  IN UINT32       Handle,
  IN CONST CHAR8 *Dir,            /* "s" or "r" */
  IN CONST UINT8 *Buf,
  IN UINT32       Len,
  IN UINTN        MaxBytes
  )
{
  CHAR8  Hex[QSEE_DUMP_CHUNK_BYTES * 2 + 1];
  UINTN  Off, ThisChunk, Cap;

  if (Buf == NULL || Len == 0) {
    return;
  }

  Cap = (Len < MaxBytes) ? Len : MaxBytes;

  for (Off = 0; Off < Cap; Off += QSEE_DUMP_CHUNK_BYTES) {
    ThisChunk = Cap - Off;
    if (ThisChunk > QSEE_DUMP_CHUNK_BYTES) ThisChunk = QSEE_DUMP_CHUNK_BYTES;

    HexN (Buf + Off, (UINT32)ThisChunk, ThisChunk, Hex, sizeof (Hex));
    VERBOSE ("qsee-buf | h=%u | dir=%a | off=%u | hex=%a\n",
             Handle, Dir, (UINT32)Off, Hex);
  }
}

STATIC BOOLEAN
IsOplusSecGuidName (
  IN CONST CHAR8 *AppName
  )
{
  if (AppName == NULL) {
    return FALSE;
  }

  /* Keep the cheap byte-prefix guard before the 16-byte compare. ASCII TA
   * names fail quickly; the OplusSec binary name is exactly 16 bytes. */
  if ((UINT8)AppName[0] != kOplusSecGuidBytes[0] ||
      (UINT8)AppName[1] != kOplusSecGuidBytes[1] ||
      (UINT8)AppName[2] != kOplusSecGuidBytes[2] ||
      (UINT8)AppName[3] != kOplusSecGuidBytes[3]) {
    return FALSE;
  }

  return CompareMem (AppName, kOplusSecGuidBytes,
                     sizeof (kOplusSecGuidBytes)) == 0;
}

STATIC BOOLEAN
CopyBoundedAsciiAppName (
  IN  CONST CHAR8 *AppName,
  OUT CHAR8       *Out,
  IN  UINTN        OutSize
  )
{
  UINTN i;

  if (Out == NULL || OutSize == 0) {
    return FALSE;
  }
  Out[0] = '\0';

  if (AppName == NULL) {
    return FALSE;
  }

  for (i = 0; i < QSEE_APPNAME_MAX && i + 1 < OutSize; i++) {
    CHAR8 Ch = AppName[i];
    if (Ch == '\0') {
      Out[i] = '\0';
      return i > 0;
    }
    if ((UINT8)Ch < 0x20 || (UINT8)Ch > 0x7e) {
      Out[0] = '\0';
      return FALSE;
    }
    Out[i] = Ch;
  }

  Out[0] = '\0';
  return FALSE;
}

/* Heuristic: a SendCmd payload that's a multiple-of-page (4096 + 4 hdr is
 * the canonical Phoenix shape) wants the wider dump. Cheap, evidence-based,
 * doesn't require knowing the TA name. */
STATIC BOOLEAN
IsPhoenixShape (
  IN UINT32 Handle,
  IN UINT32 SendLen,
  IN UINT32 RspLen
  )
{
  if (Handle == gPhoenixHandle) return TRUE;
  if (SendLen >= 4096 || RspLen >= 4096) return TRUE;
  return FALSE;
}

/* Read a u32 little-endian from Buf[Off..Off+4) if in bounds; else 0.
 * Bounds expressed as `Off > Len || Len - Off < 4` to avoid integer
 * overflow if `Off + 4` wraps past UINT32_MAX (guard would silently
 * pass on a near-overflow Off and read past the buffer). */
STATIC UINT32
ReadU32At (
  IN CONST UINT8 *Buf,
  IN UINT32       Len,
  IN UINT32       Off
  )
{
  UINT32 V = 0;
  if (Buf == NULL || Off > Len || Len - Off < 4) return 0;
  CopyMem (&V, Buf + Off, sizeof (V));
  return V;
}

/** KeyMaster TA cmd-id structured decoder.

    Cmd-id space: KEYMASTER_UTILS_CMD_ID = 0x200 + N. Wire-format definitions
    cross-validated against ~/gbl_root_canoe/tools/keymaster_wire.h (their
    QSEECOM hook impl with confirmed mode-1 attestation behaviour).

    Phase-B mutation targets: 0x201 (SET_ROT), 0x208 (SET_BOOT_STATE),
    0x207 (SET_VERSION). 0x211 (SET_VBH) is observable but the on-device
    digest already matches a coherent locked vbmeta, so passthrough is
    fine for mode-1. 0x218 (FBE_SET_SEED) and 0x219 (GENERATE_FRS_AND_UDS)
    MUST NOT be touched — they feed FBE class-key derivation and DICE
    FRS/UDS respectively; mutation breaks /data unwrap. **/
STATIC VOID
KmDecodeKnownCmd (
  IN UINT32       CmdId,
  IN UINT32       Handle,
  IN CONST UINT8 *SendBuf,
  IN UINT32       SendLen,
  IN CONST UINT8 *RspBuf,
  IN UINT32       RspLen,
  IN EFI_STATUS   Status
  )
{
  CHAR8 Hex[65];

  switch (CmdId) {

    case 0x00000200: {
      /* Probe / get-version. Send: {cmd:u32}. Rsp: {status, ver_major,
       * ver_minor, ver_build, build_id}. Not in keymaster_wire.h's named
       * cmd table — likely an OEM probe that TZ exposes ahead of named
       * ops. */
      UINT32 RStatus = ReadU32At (RspBuf, RspLen, 0);
      UINT32 VMaj    = ReadU32At (RspBuf, RspLen, 4);
      UINT32 VMin    = ReadU32At (RspBuf, RspLen, 8);
      UINT32 VBld    = ReadU32At (RspBuf, RspLen, 12);
      UINT32 BId     = ReadU32At (RspBuf, RspLen, 16);
      VERBOSE ("qsee-km | cmd=0x%08x(probe) | h=%u | rstatus=0x%x | "
               "ver=%u.%u.%u | buildId=0x%x | st=%r\n",
               CmdId, Handle, RStatus, VMaj, VMin, VBld, BId, Status);
      (VOID)RStatus; (VOID)VMaj; (VOID)VMin; (VOID)VBld; (VOID)BId;
      break;
    }

    case 0x00000201: {
      /* SET_ROT — KmSetRotReqWire (44 B). Send: {cmd, RotOffset=12,
       * RotSize=32, RotDigest[32]}. RotDigest = SHA256(AVBPubKey ||
       * IsUnlockedByte). NOT the same as bootconfig
       * androidboot.vbmeta.public_key_digest (= SHA256(AVBPubKey) with
       * no trailing byte) — that one travels via SET_BOOT_STATE. */
      UINT32 RotOffset = ReadU32At (SendBuf, SendLen, 4);
      UINT32 RotSize   = ReadU32At (SendBuf, SendLen, 8);
      HexN (SendBuf + 12, (SendLen >= 12) ? (SendLen - 12) : 0, 32,
            Hex, sizeof (Hex));
      GBL_INFO ("qsee-km | cmd=0x%08x(SET_ROT) | h=%u | offset=%u | size=%u | "
                "rotDigest=%a | st=%r\n",
                CmdId, Handle, RotOffset, RotSize, Hex, Status);
      break;
    }

    case 0x00000202: {
      /* READ_KM_DEVICE_STATE — read query, not a mutation. */
      UINT32 AddrLo = ReadU32At (SendBuf, SendLen, 4);
      UINT32 AddrHi = ReadU32At (SendBuf, SendLen, 8);
      VERBOSE ("qsee-km | cmd=0x%08x(READ_KM_DEVICE_STATE) | h=%u | "
               "addr=0x%x_%08x | st=%r\n",
               CmdId, Handle, AddrHi, AddrLo, Status);
      (VOID)AddrLo; (VOID)AddrHi;
      break;
    }

    case 0x00000203: {
      /* WRITE_KM_DEVICE_STATE — write mutation, keep as GBL_INFO. */
      UINT32 AddrLo = ReadU32At (SendBuf, SendLen, 4);
      UINT32 AddrHi = ReadU32At (SendBuf, SendLen, 8);
      GBL_INFO ("qsee-km | cmd=0x%08x(WRITE_KM_DEVICE_STATE) | h=%u | "
                "addr=0x%x_%08x | st=%r\n",
                CmdId, Handle, AddrHi, AddrLo, Status);
      break;
    }

    case 0x00000204: {
      /* MILESTONE_CALL (4 B). Final TZ notification after BCC/pvmfw
       * setup is complete. */
      GBL_INFO ("qsee-km | cmd=0x%08x(MILESTONE_CALL) | h=%u | st=%r\n",
                CmdId, Handle, Status);
      break;
    }

    case 0x00000207: {
      /* SET_VERSION — KmSetVersionReqWire. Send: {cmd, OsVersion,
       * OsPatchLevel}. Encoding is Qcom-specific (NOT the Android
       * boot.img header form):
       *   OsVersion = (Major << 14) | (Minor << 7) | SubMinor
       *   OsPatchLevel SPL = (Day << 11) | ((Year - 2000) << 4) | Month */
      UINT32 Ver = ReadU32At (SendBuf, SendLen, 4);
      UINT32 Spl = ReadU32At (SendBuf, SendLen, 8);
      GBL_INFO ("qsee-km | cmd=0x%08x(SET_VERSION) | h=%u | osVer=0x%x | "
                "spl=0x%x | st=%r\n",
                CmdId, Handle, Ver, Spl, Status);
      break;
    }

    case 0x00000208: {
      /* SET_BOOT_STATE — KmSetBootStateReqWire (64 B). Send:
       *   {cmd, Version, Offset=16, Size=48,
       *    BootState{IsUnlocked, PublicKey[32], Color, SystemVersion,
       *              SystemSecurityLevel}}
       * Color: 0=GREEN, 1=YELLOW, 2=ORANGE, 3=RED. */
      UINT32 Version = ReadU32At (SendBuf, SendLen, 4);
      UINT32 Offset  = ReadU32At (SendBuf, SendLen, 8);
      UINT32 Size    = ReadU32At (SendBuf, SendLen, 12);
      UINT32 Unlk    = ReadU32At (SendBuf, SendLen, 16);
      UINT32 Color   = ReadU32At (SendBuf, SendLen, 16 + 4 + 32);
      UINT32 SysVer  = ReadU32At (SendBuf, SendLen, 16 + 4 + 32 + 4);
      UINT32 SysSpl  = ReadU32At (SendBuf, SendLen, 16 + 4 + 32 + 8);
      HexN (SendBuf + 20, (SendLen >= 20) ? (SendLen - 20) : 0, 32,
            Hex, sizeof (Hex));
      GBL_INFO ("qsee-km | cmd=0x%08x(SET_BOOT_STATE) | h=%u | ver=%u | "
                "offset=%u | size=%u | isUnlocked=%u | pubKey=%a | "
                "color=%u | sysVer=0x%x | sysSpl=0x%x | st=%r\n",
                CmdId, Handle, Version, Offset, Size, Unlk, Hex, Color,
                SysVer, SysSpl, Status);
      break;
    }

    case 0x00000211: {
      /* SET_VBH — KmSetVbhReqWire (36 B). Send: {cmd, Vbh[32]}.
       * Vbh = on-device vbmeta digest. */
      HexN (SendBuf + 4, (SendLen >= 4) ? (SendLen - 4) : 0, 32,
            Hex, sizeof (Hex));
      GBL_INFO ("qsee-km | cmd=0x%08x(SET_VBH) | h=%u | vbh=%a | st=%r\n",
                CmdId, Handle, Hex, Status);
      break;
    }

    case 0x00000218: {
      /* FBE_SET_SEED. Sends FBE class-key derivation seed to TZ.
       * MODE-1: DO NOT mutate. */
      VERBOSE ("qsee-km | cmd=0x%08x(FBE_SET_SEED) | h=%u | sl=%u | "
               "st=%r | DO-NOT-MUTATE\n",
               CmdId, Handle, SendLen, Status);
      break;
    }

    case 0x00000219: {
      /* GENERATE_FRS_AND_UDS — KmGetFrsUdsReqWire. Fetches DICE FRS
       * (Factory-Reset Secret) + UDS (Unique Device Secret). UDS is a
       * real per-device secret on a properly-provisioned chip; the 0x0F
       * bytes seen in the request are bootloader-side input
       * (FrsSec[32]), not response sentinel. Response interpretation
       * needs ground-truth from a stock-locked capture (open
       * follow-up). MODE-1: DO NOT mutate regardless — DICE/BCC chain
       * is downstream of this output. */
      UINT32 FdrFlag   = ReadU32At (SendBuf, SendLen, 4);
      UINT32 FrsSecLen = ReadU32At (SendBuf, SendLen, 8);
      VERBOSE ("qsee-km | cmd=0x%08x(GENERATE_FRS_AND_UDS) | h=%u | "
               "fdrFlag=0x%x | frsSecLen=%u | st=%r | DO-NOT-MUTATE\n",
               CmdId, Handle, FdrFlag, FrsSecLen, Status);
      (VOID)FdrFlag; (VOID)FrsSecLen;
      break;
    }

    default:
      /* Unknown cmd — skip the decoded line. The generic qsee line
       * already covers the raw byte view. */
      break;
  }
}

STATIC EFI_STATUS EFIAPI
HookedStartApp (
  IN  QCOM_QSEECOM_PROTOCOL *This,
  IN  CHAR8                 *AppName,
  OUT UINT32                *Handle
  )
{
  EFI_STATUS Status;
  BOOLEAN    First;
  UINT32     OutHandle = 0;
  BOOLEAN    IsOplusSec;
  BOOLEAN    HasAsciiName;
  CHAR8      AppNameAscii[QSEE_APPNAME_MAX];

  First = HookEnter (&gQseecomStartGuard);

  if (gOriginalStartApp == NULL) {
    HookLeave (&gQseecomStartGuard);
    return EFI_NOT_READY;
  }

  if (!First) {
    Status = gOriginalStartApp (This, AppName, Handle);
    HookLeave (&gQseecomStartGuard);
    return Status;
  }

  Status = gOriginalStartApp (This, AppName, Handle);
  if (Handle != NULL) {
    OutHandle = *Handle;
  }

  IsOplusSec  = IsOplusSecGuidName (AppName);
  HasAsciiName = CopyBoundedAsciiAppName (AppName, AppNameAscii,
                                          sizeof (AppNameAscii));

  if (HasAsciiName) {
    GBL_INFO ("qsee-start | app=\"%a\" | h=%u | st=%r\n",
              AppNameAscii, OutHandle, Status);
  } else if (IsOplusSec) {
    GBL_INFO ("qsee-start | app=<OplusSec-GUID> | h=%u | st=%r\n",
              OutHandle, Status);
  } else {
    GBL_INFO ("qsee-start | app=<non-ascii-or-unbounded> | h=%u | st=%r\n",
              OutHandle, Status);
  }

  /* Heuristic name → "Phoenix-shaped" tag. Anything OnePlus-flavored that
   * isn't a stock Qcom TA gets the wide dump. Update list as we identify
   * more TAs. */
  if (!EFI_ERROR (Status) && AppName != NULL) {
    if (HasAsciiName &&
        (AsciiStrCmp (AppNameAscii, "oplusreserve")  == 0 ||
         AsciiStrCmp (AppNameAscii, "oplusreserve1") == 0 ||
         AsciiStrCmp (AppNameAscii, "oplus_phoenix") == 0 ||
         AsciiStrCmp (AppNameAscii, "phoenix")       == 0)) {
      gPhoenixHandle = OutHandle;
      GBL_INFO ("qsee-start: tagged \"%a\" h=%u as Phoenix-shaped\n",
                AppNameAscii, OutHandle);
    }

    /* OplusSec: AppName is a 16-byte EFI_GUID, not ASCII (Ghidra G1).
     * Gate the 16-byte CompareMem behind byte-by-byte prefix checks.
     * A u32 cast on a CHAR8* would (a) issue an unaligned load on
     * AArch64 and (b) still cross a page boundary if the ASCII name
     * is short — exactly what we want to avoid. Byte loads short-
     * circuit on the first mismatch and never read past the failing
     * byte. No ASCII TA name starts with bytes 6A DA 1D E1, so this
     * is exact. */
    if (IsOplusSec) {
      gOplusSecHandle = OutHandle;
      GBL_INFO ("qsee-start: tagged OplusSec h=%u "
                "(GUID E11DDA6A-651B-4AB4-B8C5-30B352B472E2)\n",
                OutHandle);
    }
  }

  HookLeave (&gQseecomStartGuard);
  return Status;
}

STATIC EFI_STATUS EFIAPI
HookedSendCmd (
  IN     QCOM_QSEECOM_PROTOCOL *This,
  IN     UINT32                 Handle,
  IN     UINT8                 *SendBuf,
  IN     UINT32                 SendLen,
  IN OUT UINT8                 *RspBuf,
  IN     UINT32                 RspLen
  )
{
  EFI_STATUS Status;
  BOOLEAN    First;
  BOOLEAN    Wide;
  UINT32     CmdId = 0;
  CHAR8      SendHex[65];
  CHAR8      RspHex[65];

  First = HookEnter (&gQseecomSendGuard);

  if (gOriginalSendCmd == NULL) {
    HookLeave (&gQseecomSendGuard);
    return EFI_NOT_READY;
  }

  if (!First) {
    Status = gOriginalSendCmd (This, Handle, SendBuf, SendLen, RspBuf, RspLen);
    HookLeave (&gQseecomSendGuard);
    return Status;
  }

  Wide = IsPhoenixShape (Handle, SendLen, RspLen);

  if (SendBuf != NULL && SendLen >= sizeof (UINT32)) {
    CopyMem (&CmdId, SendBuf, sizeof (CmdId));
  }

  /* Universal policy: drop certain OplusSec commands before forwarding. */
  if (Handle == gOplusSecHandle && Handle != (UINT32)-1) {
    EFI_STATUS FakeStatus;
    if (UniversalPolicy_ShouldDropQseeOplusSec (CmdId, &FakeStatus)) {
      HookLeave (&gQseecomSendGuard);
      return FakeStatus;
    }
  }

  Status = gOriginalSendCmd (This, Handle, SendBuf, SendLen, RspBuf, RspLen);

  /* For non-wide traffic, keep the single-line summary with inline 32 B
   * prefixes. For wide traffic, emit a header line + chunked continuation
   * lines so DEBUG()'s 256-char buffer never truncates mid-hex. */
  if (!Wide) {
    HexN (SendBuf, SendLen, 32, SendHex, sizeof (SendHex));
    HexN (RspBuf,  RspLen,  32, RspHex,  sizeof (RspHex));
    VERBOSE ("qsee | cmd=0x%08x | h=%u | sl=%u | s32=%a | rl=%u | r32=%a | st=%r\n",
             CmdId, Handle,
             SendLen, SendHex,
             RspLen,  RspHex,
             Status);
  } else {
    VERBOSE ("qsee | cmd=0x%08x | h=%u | sl=%u | rl=%u | st=%r | wide\n",
             CmdId, Handle, SendLen, RspLen, Status);
    DumpChunked (Handle, "s", SendBuf, SendLen, 192);
    DumpChunked (Handle, "r", RspBuf,  RspLen,  192);
  }

  /* T1.5: KeyMaster cmd-id structured decoder. Emits one extra
   * qsee-km line for documented cmds (0x200/0x201/0x208/0x211/0x219);
   * silent for everything else. The generic qsee line above is the
   * source-of-truth for raw bytes; this is interpretation. */
  KmDecodeKnownCmd (CmdId, Handle, SendBuf, SendLen, RspBuf, RspLen, Status);

  /* OplusSec cmd-id decoder, gated by handle (Ghidra G1).
   * Cmd-ids occupy a low integer space and would collide with KeyMaster
   * 0x200+ cmds if dispatched globally. */
  if (Handle == gOplusSecHandle && Handle != (UINT32)-1) {
    CONST CHAR8 *Name = NULL;
    switch (CmdId) {
      case 0x00000004: Name = "GetVersion";          break;
      case 0x00000009: Name = "read_rpmb_boot_info"; break;
      case 0x0000000A: Name = "write_rpmb_boot_info";break;
      default:         Name = NULL;                   break;
    }
    if (Name != NULL) {
      VERBOSE ("qsee-oplussec | cmd=0x%02x(%a) | h=%u | sl=%u | rl=%u | st=%r\n",
               CmdId, Name, Handle, SendLen, RspLen, Status);
    } else {
      VERBOSE ("qsee-oplussec | cmd=0x%02x(unknown) | h=%u | sl=%u | rl=%u | st=%r\n",
               CmdId, Handle, SendLen, RspLen, Status);
    }
  }

  HookLeave (&gQseecomSendGuard);
  return Status;
}

EFI_STATUS
InstallQseecomHook (VOID)
{
  EFI_STATUS              Status;
  QCOM_QSEECOM_PROTOCOL  *Qseecom = NULL;

  if (gHookedProtocol != NULL) {
    return EFI_ALREADY_STARTED;
  }

  Status = gBS->LocateProtocol (&gQcomQseecomProtocolGuid, NULL,
                                (VOID **)&Qseecom);
  if (EFI_ERROR (Status) || Qseecom == NULL) {
    Print (L"QseecomHook: LocateProtocol failed: %r\n", Status);
    return Status;
  }

  if (Qseecom->QseecomSendCmd == NULL || Qseecom->QseecomStartApp == NULL) {
    Print (L"QseecomHook: SendCmd or StartApp slot is NULL\n");
    return EFI_NOT_READY;
  }

  gOriginalStartApp        = Qseecom->QseecomStartApp;
  gOriginalSendCmd         = Qseecom->QseecomSendCmd;
  Qseecom->QseecomStartApp = HookedStartApp;
  Qseecom->QseecomSendCmd  = HookedSendCmd;
  gHookedProtocol          = Qseecom;

  GBL_INFO ("QseecomHook: installed StartApp=%p SendCmd=%p (orig start=%p send=%p)\n",
            HookedStartApp, HookedSendCmd, gOriginalStartApp, gOriginalSendCmd);
  return EFI_SUCCESS;
}
