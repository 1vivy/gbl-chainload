/** @file ScmHook.c — verbose hook for QCOM_SCM_PROTOCOL.

  Wraps the five data-carrying vtable slots of `gQcomScmProtocolGuid`:

      ScmSysCall      (slot 1) — generic syscall, opaque Cmd buffer
      ScmFastCall2    (slot 2) — fast SMC with 2 parameters
      ScmSipSysCall   (slot 7) — SIP-owner SMC; 10×u64 params + 4×u64 results
      ScmSendCommand  (slot 5) — TA dispatch (start/send/shutdown/...)
      ScmQseeSysCall  (slot 10) — QSEE-OS SMC; same param shape as SIP

  Control-plane slots (GetVersion, Register/DeregisterCallback,
  ExitBootServices, GetClientEnv) are intentionally not hooked — they're
  init/shutdown plumbing, low signal vs noise.

  Each wrapper logs ONE structured line per first-entry call, then passes
  through to the saved original. All five share a single reentry guard
  because internal SCM traffic kicked off from our logging path (LogFs →
  SimpleFS → block IO → SCM) can land on ANY slot, so per-slot guards
  would still recurse. Shared `gScmGuard` collapses every nested SCM
  dispatch into a silent pass-through until the outer call returns.

  No request/response mutation. Mutation lives in Phase B/C libs.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/EFIScm.h>
#include "HookCommon.h"
#include "UniversalBaseline.h"

STATIC QCOM_SCM_PROTOCOL     *gHookedScm          = NULL;
STATIC QCOM_SCM_SYS_CALL      gOrigScmSysCall     = NULL;
STATIC QCOM_SCM_FAST_CALL_2   gOrigScmFastCall2   = NULL;
STATIC QCOM_SCM_SIP_SYS_CALL  gOrigScmSipSysCall  = NULL;
STATIC QCOM_SCM_SEND_COMMAND  gOrigScmSendCommand = NULL;
STATIC QCOM_SCM_QSEE_SYS_CALL gOrigScmQseeSysCall = NULL;

/* One guard for all five slots — see file header for rationale. */
HOOK_REENTRY_DEFINE (gScmGuard);

/** AppCmdType → short name. Mirrors the enum order in EFIScm.h. **/
STATIC CONST CHAR8 *
ScmSendCmdName (
  IN UINT32  Cmd
  )
{
  switch (Cmd) {
    case 0x01: return "start-app";
    case 0x02: return "shutdown";
    case 0x03: return "query-appid";
    case 0x04: return "register-listener";
    case 0x05: return "deregister-listener";
    case 0x06: return "send-data";
    case 0x07: return "listener-rsp";
    case 0x08: return "load-elf";
    case 0x09: return "unload-elf";
    case 0x0A: return "get-state";
    case 0x0B: return "load-serv";
    case 0x0C: return "unload-serv";
    case 0x0D: return "region-notify";
    case 0x0E: return "register-log-buf";
    case 0x0F: return "rpmb-provision";
    case 0x10: return "rpmb-erase";
    case 0x11: return "rpmb-prov-status";
    case 0x12: return "query-embedded";
    case 0x13: return "start-embedded";
    case 0x14: return "tztest-app";
    default:   return "?";
  }
}

/** Format up-to-MaxBytes bytes of Buf as lowercase hex into Out.
    Out must be >= MaxBytes*2 + 1. Always NUL-terminates. Local
    duplicate of QseecomHook's HexN — keeps the lib self-contained
    until we factor out a shared utility header. **/
STATIC VOID
ScmHexN (
  IN  CONST UINT8 *Buf,
  IN  UINT32       Len,
  IN  UINTN        MaxBytes,
  OUT CHAR8       *Out,
  IN  UINTN        OutSize
  )
{
  STATIC CONST CHAR8 kHex[] = "0123456789abcdef";
  UINTN Take, i, j;

  if (Out == NULL || OutSize == 0) {
    return;
  }
  Out[0] = '\0';
  if (Buf == NULL) {
    AsciiStrCpyS (Out, OutSize, "(null)");
    return;
  }
  if (Len == 0) {
    AsciiStrCpyS (Out, OutSize, "(empty)");
    return;
  }

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

/* -----------------------------------------------------------------------
 * Decoder A — MinkIPC / SMCI ObjectInvoke on top of ScmSendCommand.
 *
 * When ABL uses Transport B (secretkeeper, RKP/BCC, KM v2), it routes
 * through ScmGetClientEnv → IClientEnvOpen(CAppClient_UID=0x97) →
 * IAppClientGetAppObject → ObjectInvokeFunc.  The outermost SCM hop
 * that carries the TA payload lands in ScmSendCommand with CmdId=0x06
 * (send-data).  The request buffer at that point contains a flat
 * ObjectInvoke descriptor: [ObjectId:u32][OpId:u32][ArgCounts:u32]...
 *
 * Wire layout (observed from SmciInvokeUtils.h + KeymasterClient.c):
 *   Bytes 0-3   ObjectId  (uint32) — UID of the target interface
 *   Bytes 4-7   OpId      (uint32) — method index (0xFFFF = Release/Retain)
 *   Bytes 8-11  ArgCounts (uint32) — packed in/out buffer/object counts
 *     ArgCounts = (nBufsIn & 0xF) | ((nBufsOut & 0xF)<<4)
 *               | ((nObjsIn & 0xF)<<8) | ((nObjsOut & 0xF)<<12)
 *
 * TODO(ghidra): confirm exact flat-buffer layout by tracing
 * ObjectInvokeFunc in LinuxLoader_infiniti.efi.  The layout above
 * matches SmciInvokeUtils.h but the SCM-level serialisation (how
 * ObjectInvoke frames ride inside a send-data Req buffer) has not been
 * directly observed on canoe.  If the first field is NOT ObjectId
 * (e.g. there is a header length prefix), adjust the offsets below.
 * -------------------------------------------------------------------- */

/* Known ObjectId → name table (CAppClient_UID from KeymasterClient.c). */
STATIC CONST CHAR8 *
MinkObjName (
  IN UINT32  ObjId
  )
{
  switch (ObjId) {
    /* Mink service-class UIDs (Ghidra G2/G3 on LinuxLoader_infiniti.efi).
     * Note: secretkeeper / rkp-bcc are NOT ObjectIds — they're TA names
     * passed to IAppClient.getAppObject AFTER IClientEnvOpen(uid=0x97). */
    case 0x96:  return "IClientEnv";
    case 0x97:  return "CAppClient";  /* secretkeeper path on canoe */
    case 0x1A4: return "CAppLoader";  /* RKP/BCC reader path (Ghidra G3) */
    default:    return NULL;
  }
}

/* Known OpId → name table, keyed by (ObjId, OpId). */
STATIC CONST CHAR8 *
MinkOpName (
  IN UINT32  ObjId,
  IN UINT32  OpId
  )
{
  /* IAppClient (0x97 / CAppClient) */
  if (ObjId == 0x97) {
    switch (OpId) {
      case 0:      return "getAppObject";
      case 0xFFFE: return "Retain";
      case 0xFFFF: return "Release";
      default:     return NULL;
    }
  }
  /* IClientEnv (0x96) */
  if (ObjId == 0x96) {
    switch (OpId) {
      case 0:      return "open";
      default:     return NULL;
    }
  }
  /* secretkeeper / rkp-bcc — OpId space not yet confirmed; TODO(ghidra). */
  return NULL;
}

/**
  DecodeMinkIpcInvoke — attempt to parse an ObjectInvoke descriptor from
  a ScmSendCommand send-data (cmd 0x06) buffer and emit a structured log
  line.  Falls through (no log) if the buffer is too short or the fields
  look implausible.

  Observation only — no mutation.
**/
STATIC VOID
DecodeMinkIpcInvoke (
  IN CONST UINT8 *Buf,
  IN UINTN        Len
  )
{
  UINT32         ObjId, OpId, ArgCounts;
  UINT32         NBI, NBO, NOI, NOO;
  CONST CHAR8   *ObjStr;
  CONST CHAR8   *OpStr;
  CHAR8          ObjBuf[24];
  CHAR8          OpBuf[24];

  /* Minimum: ObjectId(4) + OpId(4) + ArgCounts(4) = 12 bytes. */
  if (Buf == NULL || Len < 12) {
    return;
  }

  /* Read fields little-endian. */
  ObjId     = (UINT32)Buf[0] | ((UINT32)Buf[1] << 8)
            | ((UINT32)Buf[2] << 16) | ((UINT32)Buf[3] << 24);
  OpId      = (UINT32)Buf[4] | ((UINT32)Buf[5] << 8)
            | ((UINT32)Buf[6] << 16) | ((UINT32)Buf[7] << 24);
  ArgCounts = (UINT32)Buf[8] | ((UINT32)Buf[9] << 8)
            | ((UINT32)Buf[10] << 16) | ((UINT32)Buf[11] << 24);

  /* Sanity: OpId either a small method index (< 0x1000) or
   * a Release/Retain sentinel (0xFFFE / 0xFFFF after masking low 16 bits). */
  if (OpId > 0xFFFF && OpId != 0xFFFFFFFEu && OpId != 0xFFFFFFFFu) {
    return;
  }
  /* Sanity: ArgCounts upper 16 bits must be zero (reserved). */
  if (ArgCounts >> 16) {
    return;
  }

  NBI = (ArgCounts      ) & 0xF;
  NBO = (ArgCounts >>  4) & 0xF;
  NOI = (ArgCounts >>  8) & 0xF;
  NOO = (ArgCounts >> 12) & 0xF;

  ObjStr = MinkObjName (ObjId);
  OpStr  = MinkOpName  (ObjId, OpId);

  if (ObjStr != NULL) {
    AsciiSPrint (ObjBuf, sizeof (ObjBuf), "0x%x(%a)", ObjId, ObjStr);
  } else {
    AsciiSPrint (ObjBuf, sizeof (ObjBuf), "0x%x", ObjId);
  }
  if (OpStr != NULL) {
    AsciiSPrint (OpBuf, sizeof (OpBuf), "0x%x(%a)", OpId, OpStr);
  } else {
    AsciiSPrint (OpBuf, sizeof (OpBuf), "0x%x", OpId);
  }

  DEBUG ((DEBUG_INFO,
          "mink | obj=%a | op=%a | argc=%u/%u obj=%u/%u\n",
          ObjBuf, OpBuf, NBI, NBO, NOI, NOO));
}

/* -----------------------------------------------------------------------
 * Decoder B — SCM SIP smcid ladder for ScmSipSysCall.
 *
 * Covers the ~8 SmcIds ABL actually fires during locked boot.
 * Authoritative values computed from scm_sip_interface.h macros:
 *   TZ_SYSCALL_CREATE_SMC_ID(o, s, f) = ((o&0x3f)<<24) | ((s&0xff)<<8) | (f&0xff)
 *
 * TZ_INFO_GET_SECURE_STATE      = SMC(SIP=2, INFO=6, 4)   = 0x02000604
 * TZ_BLOW_SW_FUSE_ID            = SMC(SIP=2, FUSE=8, 1)   = 0x02000801
 *   — DROP CANDIDATE (log only here; see gbl_root scm_hook.h:84-96)
 * TZ_IS_SW_FUSE_BLOWN_ID        = SMC(SIP=2, FUSE=8, 4)   = 0x02000804
 *   — NOTE: task doc used 0x02000402 but header says FUSE=8 → 0x02000804
 * TZ_INFO_GET_FEATURE_VERSION_ID= SMC(SIP=2, INFO=6, 3)   = 0x02000603
 * TZ_UPDATE_ROLLBACK_VERSION_ID = SMC(SIP=2, BOOT=1, 0x1E)= 0x0200011E
 *   — DROP CANDIDATE for re-flashability (gbl_root KM_BLOCK_TZ_ROLLBACK)
 * TZ_UPDATE_ROLLBACK_VERSION_IF_A_B_PARTITION_FEATURE_ENABLED_ID
 *                               = SMC(QSEE_OS=50,APP_MGR=1,0x10)= 0x32000110
 *   — DROP CANDIDATE same reason
 * -------------------------------------------------------------------- */

#define SCM_SIP_TZ_INFO_GET_SECURE_STATE    0x02000604u
#define SCM_SIP_TZ_BLOW_SW_FUSE_ID         0x02000801u  /* dropped universally — see UniversalBaseline.c */
#define SCM_SIP_TZ_IS_SW_FUSE_BLOWN_ID     0x02000804u
#define SCM_SIP_TZ_INFO_GET_FEATURE_VER    0x02000603u
#define SCM_SIP_TZ_UPDATE_ROLLBACK_VER     0x0200011Eu  /* DROP CANDIDATE */
#define SCM_SIP_TZ_UPDATE_ROLLBACK_VER_AB  0x32000110u  /* DROP CANDIDATE */

/* Fuse-state bit indices in status_0 returned by TZ_INFO_GET_SECURE_STATE
 * (matches avb_util.c #defines and gbl_root scm_hook.h:44-48). */
#define SCM_FUSE_SECBOOT          0u
#define SCM_FUSE_SHK              1u
#define SCM_FUSE_DEBUG_DISABLED   2u
#define SCM_FUSE_RPMB_ENABLED     5u
#define SCM_FUSE_DEBUG_RE_ENABLED 6u

/**
  DecodeSipSmcId — emit a named, structured log line for known SmcIds
  that ABL fires during locked boot.  Parameters and Results must
  already be captured before this call.  Returns TRUE if a named line
  was emitted (caller should suppress the generic fallback), FALSE to
  let the caller emit the generic scm-sip line.

  Observation only — no mutation.  Drop candidates are annotated in
  comments; actual dropping is a Phase B concern.
**/
STATIC BOOLEAN
DecodeSipSmcId (
  IN  UINT32        SmcId,
  IN  UINT32        ParamId,
  IN  CONST UINT64 *Parameters,
  IN  CONST UINT64 *Results,
  IN  EFI_STATUS    Status
  )
{
  UINT64  P0 = (Parameters != NULL) ? Parameters[0] : 0;
  UINT64  R0 = (Results    != NULL) ? Results[0]    : 0;
  UINT64  R1 = (Results    != NULL) ? Results[1]    : 0;
  UINT64  R2 = (Results    != NULL) ? Results[2]    : 0;

  switch (SmcId) {

    case SCM_SIP_TZ_INFO_GET_SECURE_STATE:
      /* R[0]=common_rsp.status (1=ok), R[1]=status_0 (fuse bitfield),
       * R[2]=status_1.  status_0 bits: SECBOOT[0] SHK[1] DEBUG_DIS[2]
       * RPMB[5] DEBUG_RE[6].  Mirror of gbl_root LogSecurityState(). */
      DEBUG ((DEBUG_INFO,
              "scm-sip | smcid=0x%08x(TZ_INFO_GET_SECURE_STATE)"
              " | tz_st=%llu | secure_state=0x%08llx | status_1=0x%08llx"
              " | secboot=%u shk=%u dbg_dis=%u rpmb=%u dbg_re=%u"
              " | st=%r\n",
              SmcId, R0, R1, R2,
              (UINT32)((R1 >> SCM_FUSE_SECBOOT)          & 1u),
              (UINT32)((R1 >> SCM_FUSE_SHK)              & 1u),
              (UINT32)((R1 >> SCM_FUSE_DEBUG_DISABLED)   & 1u),
              (UINT32)((R1 >> SCM_FUSE_RPMB_ENABLED)     & 1u),
              (UINT32)((R1 >> SCM_FUSE_DEBUG_RE_ENABLED)  & 1u),
              Status));
      return TRUE;

    case SCM_SIP_TZ_BLOW_SW_FUSE_ID:
      /* P[0]=FuseId (0=TZ_HLOS_IMG_TAMPER, 23=TZ_HLOS_TAMPER_NOTIFY).
       * This SMC is dropped by UniversalPolicy_ShouldDropScmSip before
       * reaching this decoder — this case is retained for completeness
       * but will not fire for the dropped SIP. */
      DEBUG ((DEBUG_INFO,
              "scm-sip | smcid=0x%08x(TZ_BLOW_SW_FUSE_ID)"
              " | fuse_id=0x%llx | st=%r\n",
              SmcId, P0, Status));
      return TRUE;

    case SCM_SIP_TZ_IS_SW_FUSE_BLOWN_ID:
      /* P[0]=FuseId, R[0]=status, R[1]=blown(0/1). */
      DEBUG ((DEBUG_INFO,
              "scm-sip | smcid=0x%08x(TZ_IS_SW_FUSE_BLOWN_ID)"
              " | fuse_id=0x%llx | blown=%llu | st=%r\n",
              SmcId, P0, R1, Status));
      return TRUE;

    case SCM_SIP_TZ_INFO_GET_FEATURE_VER:
      /* P[0]=feature_id (TZ_FVER_QSEE=10 gates rollback path),
       * R[0]=status, R[1]=version. */
      DEBUG ((DEBUG_INFO,
              "scm-sip | smcid=0x%08x(TZ_INFO_GET_FEATURE_VERSION_ID)"
              " | feature_id=0x%llx | version=0x%llx | st=%r\n",
              SmcId, P0, R1, Status));
      return TRUE;

    case SCM_SIP_TZ_UPDATE_ROLLBACK_VER:
      /* Anti-rollback index bump (legacy path, MajorVersion<=5).
       * DROP CANDIDATE for re-flashability — see gbl_root
       * KM_BLOCK_TZ_ROLLBACK scaffold in scm_hook.h:55-72. */
      DEBUG ((DEBUG_INFO,
              "scm-sip | smcid=0x%08x(TZ_UPDATE_ROLLBACK_VERSION_ID)"
              " | p0=0x%llx | DROP-CANDIDATE(NOT-DROPPED) | st=%r\n",
              SmcId, P0, Status));
      return TRUE;

    case SCM_SIP_TZ_UPDATE_ROLLBACK_VER_AB:
      /* Anti-rollback bump (newer A/B-aware path, MajorVersion>5).
       * DROP CANDIDATE same reason as above. */
      DEBUG ((DEBUG_INFO,
              "scm-sip | smcid=0x%08x"
              "(TZ_UPDATE_ROLLBACK_VERSION_IF_AB_PARTITION_FEATURE_ENABLED_ID)"
              " | p0=0x%llx | DROP-CANDIDATE(NOT-DROPPED) | st=%r\n",
              SmcId, P0, Status));
      return TRUE;

    default:
      return FALSE;
  }
}

/* ---------------- wrappers ---------------- */

STATIC EFI_STATUS EFIAPI
HookedScmSysCall (
  IN     QCOM_SCM_PROTOCOL *This,
  IN OUT CONST VOID         *Cmd
  )
{
  EFI_STATUS Status;
  BOOLEAN    First;

  First = HookEnter (&gScmGuard);

  if (gOrigScmSysCall == NULL) {
    HookLeave (&gScmGuard);
    return EFI_NOT_READY;
  }

  if (!First) {
    Status = gOrigScmSysCall (This, Cmd);
    HookLeave (&gScmGuard);
    return Status;
  }

  Status = gOrigScmSysCall (This, Cmd);

  /* Cmd is intentionally opaque (caller-defined struct). We log only
   * the dispatch occurrence + status; structural decode requires
   * caller-side context we don't have here. */
  DEBUG ((DEBUG_INFO, "scm-sys | cmd=%p | st=%r\n", Cmd, Status));

  HookLeave (&gScmGuard);
  return Status;
}

STATIC EFI_STATUS EFIAPI
HookedScmFastCall2 (
  IN QCOM_SCM_PROTOCOL *This,
  IN UINT32             Id,
  IN UINT32             Param0,
  IN UINT32             Param1
  )
{
  EFI_STATUS Status;
  BOOLEAN    First;

  First = HookEnter (&gScmGuard);

  if (gOrigScmFastCall2 == NULL) {
    HookLeave (&gScmGuard);
    return EFI_NOT_READY;
  }

  if (!First) {
    Status = gOrigScmFastCall2 (This, Id, Param0, Param1);
    HookLeave (&gScmGuard);
    return Status;
  }

  Status = gOrigScmFastCall2 (This, Id, Param0, Param1);

  DEBUG ((DEBUG_INFO,
          "scm-fast | id=0x%x | p0=0x%x | p1=0x%x | st=%r\n",
          Id, Param0, Param1, Status));

  HookLeave (&gScmGuard);
  return Status;
}

STATIC EFI_STATUS EFIAPI
HookedScmSipSysCall (
  IN  QCOM_SCM_PROTOCOL *This,
  IN  UINT32             SmcId,
  IN  UINT32             ParamId,
  IN  UINT64             Parameters[SCM_MAX_NUM_PARAMETERS],
  OUT UINT64             Results[SCM_MAX_NUM_RESULTS]
  )
{
  EFI_STATUS Status;
  BOOLEAN    First;
  UINT64     P0, P1, P2, P3;
  UINT64     R0, R1, R2, R3;

  First = HookEnter (&gScmGuard);

  if (gOrigScmSipSysCall == NULL) {
    HookLeave (&gScmGuard);
    return EFI_NOT_READY;
  }

  if (!First) {
    Status = gOrigScmSipSysCall (This, SmcId, ParamId, Parameters, Results);
    HookLeave (&gScmGuard);
    return Status;
  }

  /* Universal policy: drop certain SIPs before forwarding to TZ. */
  {
    EFI_STATUS FakeStatus;
    if (UniversalPolicy_ShouldDropScmSip (SmcId, &FakeStatus)) {
      HookLeave (&gScmGuard);
      return FakeStatus;
    }
  }

  Status = gOrigScmSipSysCall (This, SmcId, ParamId, Parameters, Results);

  /* Try the named SmcId decoder first (Decoder B).  Falls back to the
   * generic dump for any SmcId not in the ladder. */
  if (!DecodeSipSmcId (SmcId, ParamId, Parameters, Results, Status)) {
    /* Generic fallback — log first 4 of 10 params + all 4 results. */
    P0 = (Parameters != NULL) ? Parameters[0] : 0;
    P1 = (Parameters != NULL) ? Parameters[1] : 0;
    P2 = (Parameters != NULL) ? Parameters[2] : 0;
    P3 = (Parameters != NULL) ? Parameters[3] : 0;
    R0 = (Results != NULL)    ? Results[0]    : 0;
    R1 = (Results != NULL)    ? Results[1]    : 0;
    R2 = (Results != NULL)    ? Results[2]    : 0;
    R3 = (Results != NULL)    ? Results[3]    : 0;

    DEBUG ((DEBUG_INFO,
            "scm-sip | smcid=0x%08x | paramid=0x%08x"
            " | p0=0x%016lx | p1=0x%016lx | p2=0x%016lx | p3=0x%016lx"
            " | r0=0x%016lx | r1=0x%016lx | r2=0x%016lx | r3=0x%016lx"
            " | st=%r\n",
            SmcId, ParamId, P0, P1, P2, P3, R0, R1, R2, R3, Status));
  }

  HookLeave (&gScmGuard);
  return Status;
}

STATIC EFI_STATUS EFIAPI
HookedScmQseeSysCall (
  IN  QCOM_SCM_PROTOCOL *This,
  IN  UINT32             SmcId,
  IN  UINT32             ParamId,
  IN  UINT64             Parameters[SCM_MAX_NUM_PARAMETERS],
  OUT UINT64             Results[SCM_MAX_NUM_RESULTS]
  )
{
  EFI_STATUS Status;
  BOOLEAN    First;
  UINT64     P0, P1, P2, P3;
  UINT64     R0, R1, R2, R3;

  First = HookEnter (&gScmGuard);

  if (gOrigScmQseeSysCall == NULL) {
    HookLeave (&gScmGuard);
    return EFI_NOT_READY;
  }

  if (!First) {
    Status = gOrigScmQseeSysCall (This, SmcId, ParamId, Parameters, Results);
    HookLeave (&gScmGuard);
    return Status;
  }

  Status = gOrigScmQseeSysCall (This, SmcId, ParamId, Parameters, Results);

  P0 = (Parameters != NULL) ? Parameters[0] : 0;
  P1 = (Parameters != NULL) ? Parameters[1] : 0;
  P2 = (Parameters != NULL) ? Parameters[2] : 0;
  P3 = (Parameters != NULL) ? Parameters[3] : 0;
  R0 = (Results != NULL)    ? Results[0]    : 0;
  R1 = (Results != NULL)    ? Results[1]    : 0;
  R2 = (Results != NULL)    ? Results[2]    : 0;
  R3 = (Results != NULL)    ? Results[3]    : 0;

  DEBUG ((DEBUG_INFO,
          "scm-qsee | smcid=0x%08x | paramid=0x%08x"
          " | p0=0x%016lx | p1=0x%016lx | p2=0x%016lx | p3=0x%016lx"
          " | r0=0x%016lx | r1=0x%016lx | r2=0x%016lx | r3=0x%016lx"
          " | st=%r\n",
          SmcId, ParamId, P0, P1, P2, P3, R0, R1, R2, R3, Status));

  HookLeave (&gScmGuard);
  return Status;
}

STATIC EFI_STATUS EFIAPI
HookedScmSendCommand (
  IN     QCOM_SCM_PROTOCOL *This,
  IN     AppCmdType         CmdId,
  IN     UINT32            *AppId,
  IN OUT VOID              *Req,
  IN     UINTN              ReqLen,
  IN OUT VOID              *Rsp,
  IN     UINTN              RspLen
  )
{
  EFI_STATUS Status;
  BOOLEAN    First;
  CHAR8      ReqHex[65];
  CHAR8      RspHex[65];
  UINT32     AppIdValue;

  First = HookEnter (&gScmGuard);

  if (gOrigScmSendCommand == NULL) {
    HookLeave (&gScmGuard);
    return EFI_NOT_READY;
  }

  if (!First) {
    Status = gOrigScmSendCommand (This, CmdId, AppId, Req, ReqLen, Rsp, RspLen);
    HookLeave (&gScmGuard);
    return Status;
  }

  Status = gOrigScmSendCommand (This, CmdId, AppId, Req, ReqLen, Rsp, RspLen);

  ScmHexN ((CONST UINT8 *)Req, (UINT32)ReqLen, 32, ReqHex, sizeof (ReqHex));
  ScmHexN ((CONST UINT8 *)Rsp, (UINT32)RspLen, 32, RspHex, sizeof (RspHex));
  AppIdValue = (AppId != NULL) ? *AppId : 0;

  DEBUG ((DEBUG_INFO,
          "scm-send | cmd=%a(0x%x) | appid=%u | sl=%lu | s32=%a"
          " | rl=%lu | r32=%a | st=%r\n",
          ScmSendCmdName ((UINT32)CmdId), (UINT32)CmdId, AppIdValue,
          (UINT64)ReqLen, ReqHex, (UINT64)RspLen, RspHex, Status));

  /* Decoder A — if this is a send-data (0x06) dispatch, attempt to
   * parse the request buffer as a MinkIPC ObjectInvoke descriptor and
   * emit a supplementary structured line.  No-ops silently when the
   * buffer doesn't match the expected shape. */
  if ((UINT32)CmdId == 0x06) {
    DecodeMinkIpcInvoke ((CONST UINT8 *)Req, ReqLen);
  }

  HookLeave (&gScmGuard);
  return Status;
}

/* ---------------- installer ---------------- */

EFI_STATUS
InstallScmHook (VOID)
{
  EFI_STATUS         Status;
  QCOM_SCM_PROTOCOL *Scm = NULL;
  UINTN              Installed = 0;

  if (gHookedScm != NULL) {
    return EFI_ALREADY_STARTED;
  }

  Status = gBS->LocateProtocol (&gQcomScmProtocolGuid, NULL, (VOID **)&Scm);
  if (EFI_ERROR (Status) || Scm == NULL) {
    Print (L"ScmHook: LocateProtocol failed: %r\n", Status);
    return Status;
  }

  /* Per-slot null check + swap. A retail BSP may not populate every
   * slot; we install what's there and report the count. */

  if (Scm->ScmSysCall != NULL) {
    gOrigScmSysCall = Scm->ScmSysCall;
    Scm->ScmSysCall = HookedScmSysCall;
    Installed++;
  } else {
    Print (L"ScmHook: ScmSysCall slot is NULL — skipping\n");
  }

  if (Scm->ScmFastCall2 != NULL) {
    gOrigScmFastCall2 = Scm->ScmFastCall2;
    Scm->ScmFastCall2 = HookedScmFastCall2;
    Installed++;
  } else {
    Print (L"ScmHook: ScmFastCall2 slot is NULL — skipping\n");
  }

  if (Scm->ScmSipSysCall != NULL) {
    gOrigScmSipSysCall = Scm->ScmSipSysCall;
    Scm->ScmSipSysCall = HookedScmSipSysCall;
    Installed++;
  } else {
    Print (L"ScmHook: ScmSipSysCall slot is NULL — skipping\n");
  }

  if (Scm->ScmSendCommand != NULL) {
    gOrigScmSendCommand = Scm->ScmSendCommand;
    Scm->ScmSendCommand = HookedScmSendCommand;
    Installed++;
  } else {
    Print (L"ScmHook: ScmSendCommand slot is NULL — skipping\n");
  }

  if (Scm->ScmQseeSysCall != NULL) {
    gOrigScmQseeSysCall = Scm->ScmQseeSysCall;
    Scm->ScmQseeSysCall = HookedScmQseeSysCall;
    Installed++;
  } else {
    Print (L"ScmHook: ScmQseeSysCall slot is NULL — skipping\n");
  }

  gHookedScm = Scm;
  DEBUG ((DEBUG_INFO, "ScmHook: installed %u of 5 slots\n", (UINT32)Installed));
  return EFI_SUCCESS;
}
