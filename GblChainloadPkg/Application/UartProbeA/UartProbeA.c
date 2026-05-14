/** @file
  UartProbeA — throwaway EFI probe, SAFE-BY-CONSTRUCTION revision.

  Goal: answer Q1 (UefiInfoBlk HOB GUID + struct layout) without
  dereferencing any HOB-body content. Writes the full HOB inventory to
  a logfs file so we don't depend on screen scrolling.

  Q5 (where the magic lands in UefiLog<N>.txt) is INTENTIONALLY DEFERRED
  to a follow-up probe (UartProbeA2) once we know the UART buffer
  pointer from Q1 — planting the magic requires dereferencing a pointer
  we have not yet validated.

  Previous revisions (c3ef83e, 01ab84f) crashed the canoe display
  driver. Root cause: 01ab84f auto-dereferenced any 8-byte HOB body
  whose value fell in a loose DRAM range, but on canoe many 8-byte
  bodies that pass that range check are NOT valid pointers.

  This revision dereferences nothing. It only:
    - walks the HOB list (read-only)
    - reads HOB headers (HobType, HobLength) — required to advance
    - copies HOB-body bytes into a local text buffer for hex output
    - writes that buffer to \UartProbeA_Hobs.txt on the logfs partition

  Source spec: docs/re/qcom-uart-log-buffer-probe-plan.md § Probe A.
  DELETE after on-device capture.
**/

#include <Uefi.h>
#include <Pi/PiMultiPhase.h>
#include <Pi/PiHob.h>
#include <Library/UefiLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/HobLib.h>
#include <Library/TimerLib.h>
#include <Library/LogFsLib.h>

#define IBLK_SIGNATURE  0x6B6C4249u   /* 'I'|'B'<<8|'l'<<16|'k'<<24 */

/* 64 KiB of static text capture — large enough for ~100 GUID_EXT HOBs at
 * ~600 bytes per entry. BSS-resident; no heap allocations from us. */
#define CAPTURE_CAP  0x10000
STATIC CHAR8 mCapture[CAPTURE_CAP];
STATIC UINTN mCaptureUsed = 0;

STATIC VOID
EFIAPI
CapPrintf (
  IN CONST CHAR8 *Fmt,
  ...
  )
{
  VA_LIST  Marker;
  UINTN    Avail;
  UINTN    Wrote;

  if (mCaptureUsed >= CAPTURE_CAP - 1) return;
  Avail = CAPTURE_CAP - mCaptureUsed - 1;

  VA_START (Marker, Fmt);
  Wrote = AsciiVSPrint (mCapture + mCaptureUsed, Avail, Fmt, Marker);
  VA_END (Marker);

  mCaptureUsed += Wrote;
  if (mCaptureUsed >= CAPTURE_CAP) mCaptureUsed = CAPTURE_CAP - 1;
  mCapture[mCaptureUsed] = '\0';
}

STATIC VOID
CapHex (
  IN CONST UINT8 *Buf,
  IN UINTN        Len
  )
{
  UINTN  i;
  for (i = 0; i < Len; i++) {
    if ((i & 31) == 0) CapPrintf ("\n    [+0x%03x]", (UINT32)i);
    CapPrintf (" %02x", Buf[i]);
  }
  CapPrintf ("\n");
}

EFI_STATUS
EFIAPI
UartProbeAEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_PEI_HOB_POINTERS  Hob;
  EFI_STATUS            Status;
  UINTN                 HobIndex;
  UINTN                 GuidExtCount;
  UINTN                 StructCandidates;
  UINTN                 PointerCandidates;
  UINT64                Tsc;

  Tsc = GetPerformanceCounter ();

  /* Banner — confirms the probe ran. */
  Print (L"\n=== UartProbeA v3 (safe, no deref) tsc=0x%016lx ===\n", Tsc);

  /* Mount logfs early so we can write the inventory even if HOB iteration
   * has issues. LogFsInit also opens a GblChainload_BootN.txt — harmless
   * side effect; we don't write to it from here. */
  Status = LogFsInit ();
  if (EFI_ERROR (Status)) {
    Print (L"[ProbeA] LogFsInit failed: %r — falling back to screen-only.\n",
           Status);
  } else {
    Print (L"[ProbeA] LogFs mounted OK\n");
  }

  /* Header for the capture file. */
  CapPrintf ("UartProbeA v3 — HOB inventory\n");
  CapPrintf ("tsc=0x%016lx\n", Tsc);
  CapPrintf ("IBLK_SIGNATURE=0x%08x (little-endian 'IBlk' = bytes 49 42 6c 6b)\n",
             IBLK_SIGNATURE);
  CapPrintf ("\n");

  /* Get HOB list — but DO NOT dereference Hob.Raw beyond what
   * GET_NEXT_HOB / END_OF_HOB_LIST inherently require. */
  Hob.Raw = GetHobList ();
  if (Hob.Raw == NULL) {
    Print (L"[ProbeA] GetHobList returned NULL\n");
    CapPrintf ("ERROR: GetHobList returned NULL\n");
    goto write_out;
  }

  CapPrintf ("HOB list base: 0x%016lx\n\n", (UINT64)(UINTN)Hob.Raw);
  Print (L"[ProbeA] HOB list base: 0x%016lx\n", (UINT64)(UINTN)Hob.Raw);

  HobIndex          = 0;
  GuidExtCount      = 0;
  StructCandidates  = 0;
  PointerCandidates = 0;

  while (!END_OF_HOB_LIST (Hob)) {
    UINT16  HobType   = Hob.Header->HobType;
    UINT16  HobLength = Hob.Header->HobLength;

    /* Cap iterations defensively. */
    if (HobIndex > 512) {
      CapPrintf ("WARN: HobIndex exceeded 512 — bailing out of walk\n");
      break;
    }
    if (HobLength == 0 || HobLength > 0x4000) {
      CapPrintf ("WARN: HOB[%u] HobLength=%u looks bogus — bailing\n",
                 (UINT32)HobIndex, (UINT32)HobLength);
      break;
    }

    if (HobType == EFI_HOB_TYPE_GUID_EXTENSION) {
      EFI_GUID *G        = &Hob.Guid->Name;
      UINT8    *Body     = (UINT8 *)GET_GUID_HOB_DATA (Hob.Raw);
      UINT16    DataSize = GET_GUID_HOB_DATA_SIZE (Hob.Raw);
      UINT16    DumpLen;

      CapPrintf ("HOB[%03u] GUID_EXT  size=%u  GUID=%g\n",
                 (UINT32)HobIndex, (UINT32)DataSize, G);

      /* Interpretation hints — NO DEREFERENCES. */
      if (DataSize == 8) {
        /* Read body as UINT64 directly. This is the QCOM AddInfoBlkHob
         * pattern: body = pointer value. We print the value; we DO NOT
         * deref it. The user / a follow-up probe will validate the pointer
         * after seeing which value looks sensible. */
        UINT64 PtrLike = *(UINT64 *)Body;
        CapPrintf ("    [body-as-pointer-CANDIDATE] value = 0x%016lx\n",
                   PtrLike);
        PointerCandidates++;
      } else if (DataSize >= 0x48) {
        /* Read first 4 bytes — these are within the HOB body which is safe.
         * Compare against the IBlk signature. */
        UINT32 SigCandidate = *(UINT32 *)Body;
        if (SigCandidate == IBLK_SIGNATURE) {
          CapPrintf ("    *** STRUCT-CANDIDATE: first 4 bytes match 'IBlk' ***\n");
          StructCandidates++;
        }
      }

      /* Hex dump body bytes — capped at first 96. Reading from Body is
       * safe because the HOB walker already validated the bounds via
       * HobLength. */
      DumpLen = (DataSize < 96) ? DataSize : 96;
      if (DumpLen > 0) {
        CapPrintf ("  Body[0..%u]:", (UINT32)DumpLen);
        CapHex (Body, DumpLen);
      }
      CapPrintf ("\n");

      GuidExtCount++;
    }

    HobIndex++;
    Hob.Raw = GET_NEXT_HOB (Hob);
  }

  CapPrintf ("\n=== summary ===\n");
  CapPrintf ("Total HOBs visited:       %u\n", (UINT32)HobIndex);
  CapPrintf ("GUID_EXT HOBs:            %u\n", (UINT32)GuidExtCount);
  CapPrintf ("Struct-signature matches: %u  (data >= 0x48 + 1st-word == IBlk)\n",
             (UINT32)StructCandidates);
  CapPrintf ("8-byte body candidates:   %u  (potential pointer-to-struct)\n",
             (UINT32)PointerCandidates);

  Print (L"[ProbeA] HOB walk: %u total, %u GUID_EXT, %u struct-match, %u 8-byte\n",
         (UINT32)HobIndex, (UINT32)GuidExtCount,
         (UINT32)StructCandidates, (UINT32)PointerCandidates);

write_out:
  /* Write the inventory to logfs root if mount succeeded. */
  if (LogFsIsReady ()) {
    Status = LogFsWriteBlob (L"\\UartProbeA_Hobs.txt", mCapture, mCaptureUsed);
    if (EFI_ERROR (Status)) {
      Print (L"[ProbeA] LogFsWriteBlob failed: %r\n", Status);
    } else {
      Print (L"[ProbeA] Wrote %u bytes to \\UartProbeA_Hobs.txt\n",
             (UINT32)mCaptureUsed);
    }
    LogFsFlush ();
    LogFsClose ();
  } else {
    Print (L"[ProbeA] LogFs not ready — inventory NOT persisted.\n");
    Print (L"[ProbeA] Captured %u bytes; first 256:\n", (UINT32)mCaptureUsed);
    for (UINTN i = 0; i < mCaptureUsed && i < 256; i++) {
      CHAR16  C[2];
      C[0] = (CHAR16)mCapture[i];
      if (C[0] < 0x20 && C[0] != '\n') C[0] = '.';
      C[1] = 0;
      Print (L"%s", C);
    }
    Print (L"\n[ProbeA] (truncated)\n");
  }

  Print (L"\n[ProbeA] done — stalling 5s\n");
  gBS->Stall (5 * 1000 * 1000);
  return EFI_SUCCESS;
}
