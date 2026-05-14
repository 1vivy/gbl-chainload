/** @file
  UartProbeA — throwaway EFI app.

  Answers:
    Q1: Locate the UefiInfoBlk HOB (by SIGNATURE scan, not by guessing the GUID).
        Dump HOB GUID, struct layout, and the UART log buffer pointer + length.
    Q5: Which UefiLog<N>.txt slot does a planted magic land in — by writing
        the magic DIRECTLY into the UART log buffer (not ConOut).

  Strategy: enumerate all GUID-extension HOBs. For each, try two interpretations:
    (a) Body IS the struct (first 4 bytes == 'IBlk' signature, little-endian
        UINT32 = 0x6B6C4249).
    (b) Body is an 8-byte pointer to the struct (QCOM's AddInfoBlkHob pattern):
        deref *(UINT64*)body, then check first 4 bytes at that address.

  Source spec: docs/re/qcom-uart-log-buffer-probe-plan.md § Probe A
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

/* Signature 'I'|'B'<<8|'l'<<16|'k'<<24 — UEFI_INFO_BLOCK_SIGNATURE per QCOM BSP.
 * Bytes 0x49, 0x42, 0x6C, 0x6B in memory → little-endian UINT32 0x6B6C4249. */
#define IBLK_SIGNATURE   0x6B6C4249u

/* Magic the user greps for in UefiLog<N>.txt. Include a TSC so each run is
 * uniquely identifiable. */
#define MAGIC_PREFIX     "GBL_PROBE_A_UART_PLANT_"

/* Speculative offsets within UefiInfoBlk to test for UART buffer fields.
 * Per QCOM Sm8350-class BSP layout (canoe is SM8550, may differ):
 *   offset 0x00: UINT32 Signature
 *   offset 0x04: UINT32 StructVersion
 *   offset 0x08: CHAR8* RelInfo
 *   offset 0x10: UINT64 FdMemBase
 *   offset 0x18: UINT64 DDRMemSize
 *   offset 0x20: UINT64 StackBase
 *   offset 0x28: UINT64 StackSize
 *   offset 0x30: UINT64 HobBase
 *   offset 0x38: UINT64 UartLogBufferPtr   <-- candidate
 *   offset 0x40: UINT64 UartLogBufferLen   <-- candidate (could be UINT32 on some BSPs)
 * If canoe's layout differs, we'll see by dumping more of the struct. */
#define UART_PTR_OFFSET_GUESS  0x38
#define UART_LEN_OFFSET_GUESS  0x40

STATIC VOID
PrintHexBytes (
  IN CONST UINT8 *Buf,
  IN UINTN        Len
  )
{
  UINTN  i;
  for (i = 0; i < Len; i++) {
    if ((i & 15) == 0) Print (L"  [+0x%02x] ", (UINT32)i);
    Print (L"%02x ", Buf[i]);
    if ((i & 15) == 15 || i == Len - 1) Print (L"\n");
  }
}

/* Inspect a candidate UefiInfoBlk struct at the given address. Returns TRUE if
 * signature matches and the candidate is plausible. */
STATIC BOOLEAN
InspectCandidate (
  IN  CONST CHAR16 *Label,
  IN  EFI_GUID     *HobGuid,
  IN  UINTN         CandAddr
  )
{
  UINT32  Sig;
  UINT32  Ver;
  UINT64  UartPtr;
  UINT64  UartLenRaw;
  UINT32  UartLen;
  UINT8  *Bytes;

  if (CandAddr < 0x40000000UL || CandAddr > 0x800000000ULL) {
    return FALSE;  /* obviously out-of-range; skip silently */
  }

  Bytes = (UINT8 *)CandAddr;
  Sig = *(UINT32 *)(Bytes + 0);
  if (Sig != IBLK_SIGNATURE) return FALSE;

  Ver = *(UINT32 *)(Bytes + 4);
  UartPtr = *(UINT64 *)(Bytes + UART_PTR_OFFSET_GUESS);
  UartLenRaw = *(UINT64 *)(Bytes + UART_LEN_OFFSET_GUESS);
  UartLen = (UINT32)UartLenRaw;

  Print (L"\n=== UefiInfoBlk MATCH via %s ===\n", Label);
  Print (L"  HOB GUID:        %g\n", HobGuid);
  Print (L"  Struct @:        0x%016lx\n", (UINT64)CandAddr);
  Print (L"  Signature:       0x%08x ('IBlk' = 0x%08x)\n", Sig, IBLK_SIGNATURE);
  Print (L"  StructVersion:   0x%08x\n", Ver);
  Print (L"  UartLogBufferPtr (off 0x%02x): 0x%016lx\n", UART_PTR_OFFSET_GUESS, UartPtr);
  Print (L"  UartLogBufferLen (off 0x%02x): 0x%016lx\n", UART_LEN_OFFSET_GUESS, UartLenRaw);
  Print (L"\nFirst 128 bytes of struct:\n");
  PrintHexBytes (Bytes, 128);

  /* Plant magic into the UART buffer if pointer/len look sane. */
  if (UartPtr >= 0x40000000UL && UartPtr <= 0x800000000ULL &&
      UartLen >= 0x1000 && UartLen <= 0x200000) {
    UINT8  *Uart = (UINT8 *)(UINTN)UartPtr;
    CHAR8   Magic[96];
    UINTN   MagicLen;
    UINT64  Tsc;
    UINTN   End;

    Print (L"\n[ProbeA] UART buffer pointer + length look sane. Planting magic...\n");
    Print (L"First 64 bytes of UART buffer content:\n");
    PrintHexBytes (Uart, 64);

    Tsc = GetPerformanceCounter ();
    MagicLen = AsciiSPrint (Magic, sizeof (Magic),
                            MAGIC_PREFIX "%016lx_END\n", Tsc);
    if (MagicLen == 0) {
      Print (L"  AsciiSPrint failed — magic not planted\n");
    } else {
      for (End = 0; End < UartLen; End++) {
        if (Uart[End] == 0) break;
      }
      Print (L"  First NUL at offset 0x%x (%u bytes of valid content)\n",
             (UINT32)End, (UINT32)End);
      if (End + MagicLen < UartLen) {
        CopyMem (Uart + End, Magic, MagicLen);
        Print (L"  Planted %u bytes at offset 0x%x: %a", (UINT32)MagicLen, (UINT32)End, Magic);
      } else {
        Print (L"  BUFFER ALREADY FULL — content wrapped or no room; plant skipped\n");
      }
    }
  } else {
    Print (L"\n[ProbeA] UartLogBufferPtr or Len look implausible at these "
           L"offsets. Struct layout on canoe differs from SM8350 BSP guess.\n"
           L"Inspect hex dump above and identify Ptr/Len by hand.\n");
  }

  return TRUE;
}

STATIC BOOLEAN
ScanHobs (VOID)
{
  EFI_PEI_HOB_POINTERS  Hob;
  UINTN                 HobIndex = 0;
  UINTN                 GuidExtCount = 0;
  BOOLEAN               FoundAny = FALSE;

  Hob.Raw = GetHobList ();
  if (Hob.Raw == NULL) {
    Print (L"[ProbeA] GetHobList returned NULL — no HOBs visible\n");
    return FALSE;
  }
  Print (L"[ProbeA] HOB list base: 0x%016lx\n", (UINT64)(UINTN)Hob.Raw);

  while (!END_OF_HOB_LIST (Hob)) {
    if (Hob.Header->HobType == EFI_HOB_TYPE_GUID_EXTENSION) {
      EFI_GUID *G        = &Hob.Guid->Name;
      UINT8    *Body     = (UINT8 *)GET_GUID_HOB_DATA (Hob.Raw);
      UINT16    DataSize = GET_GUID_HOB_DATA_SIZE (Hob.Raw);

      /* Interpretation (a): body IS the struct. */
      if (DataSize >= 0x48 && *(UINT32 *)Body == IBLK_SIGNATURE) {
        if (InspectCandidate (L"body-is-struct", G, (UINTN)Body)) FoundAny = TRUE;
      }

      /* Interpretation (b): body is an 8-byte pointer to the struct
         (QCOM's AddInfoBlkHob pattern). */
      if (DataSize == 8) {
        UINTN PtrVal = *(UINTN *)Body;
        if (PtrVal >= 0x40000000UL && PtrVal <= 0x800000000ULL) {
          UINT32 SigAtTarget = *(UINT32 *)PtrVal;
          if (SigAtTarget == IBLK_SIGNATURE) {
            if (InspectCandidate (L"body-is-pointer", G, PtrVal)) FoundAny = TRUE;
          }
        }
      }

      GuidExtCount++;
    }
    HobIndex++;
    Hob.Raw = GET_NEXT_HOB (Hob);
  }

  Print (L"\n[ProbeA] Scan complete: %u HOBs total, %u GUID_EXT HOBs.\n",
         (UINT32)HobIndex, (UINT32)GuidExtCount);

  if (!FoundAny) {
    Print (L"[ProbeA] No HOB matched 'IBlk' signature via either interpretation.\n"
           L"        Dumping every GUID_EXT HOB body so you can identify by hand:\n\n");
    HobIndex = 0;
    Hob.Raw = GetHobList ();
    while (!END_OF_HOB_LIST (Hob)) {
      if (Hob.Header->HobType == EFI_HOB_TYPE_GUID_EXTENSION) {
        EFI_GUID *G        = &Hob.Guid->Name;
        UINT8    *Body     = (UINT8 *)GET_GUID_HOB_DATA (Hob.Raw);
        UINT16    DataSize = GET_GUID_HOB_DATA_SIZE (Hob.Raw);
        UINTN     Dump     = (DataSize < 32) ? DataSize : 32;

        Print (L"  HOB[%03u] GUID=%g size=%u  first%u:", (UINT32)HobIndex, G,
               (UINT32)DataSize, (UINT32)Dump);
        for (UINTN i = 0; i < Dump; i++) Print (L" %02x", Body[i]);
        Print (L"\n");
      }
      HobIndex++;
      Hob.Raw = GET_NEXT_HOB (Hob);
    }
  }

  return FoundAny;
}

EFI_STATUS
EFIAPI
UartProbeAEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT64  Tsc;

  Tsc = GetPerformanceCounter ();
  Print (L"\n=== ProbeA START  tsc=0x%016lx ===\n", Tsc);

  (VOID)ScanHobs ();

  Tsc = GetPerformanceCounter ();
  Print (L"=== ProbeA END    tsc=0x%016lx ===\n", Tsc);
  Print (L"\n[ProbeA] Stalling 5s — read screen, then device returns to fastboot.\n");
  gBS->Stall (5 * 1000 * 1000);
  return EFI_SUCCESS;
}
