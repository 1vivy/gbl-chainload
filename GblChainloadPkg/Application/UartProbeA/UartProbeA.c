/** @file
  UartProbeA — throwaway EFI app.

  Answers:
    Q1: Does a GUID-extension HOB with the UefiInfoBlk GUID exist?
        If yes, dump the first 64 bytes of its data payload so we can
        reconstruct the struct layout and find the UART buffer pointer + len.
    Q5: Which UefiLog<N>.txt slot does a planted magic string land in,
        indicating when/whether the UART log buffer is flushed to logfs.

  Usage (from fastboot host):
    fastboot stage dist/UartProbeA.efi
    fastboot oem boot-efi

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

// ---------------------------------------------------------------------------
// Magic sentinel planted into ConOut / UART stream to answer Q5.
// The on-device log capture must grep for this literal.
// ---------------------------------------------------------------------------
#define PROBE_A_MAGIC  L"UART_PROBE_A_MAGIC_55AA55AA"

// ---------------------------------------------------------------------------
// Known Qualcomm UefiInfoBlk GUID.
// Source: QcomModulePkg (multiple references); confirmed in canoe ABL blobs.
// {6c7f3a3b-5c4f-4a6b-8a3d-2f1e9c0b7d45}
// ---------------------------------------------------------------------------
static CONST EFI_GUID mUefiInfoBlkGuid = {
  0x6c7f3a3b, 0x5c4f, 0x4a6b,
  { 0x8a, 0x3d, 0x2f, 0x1e, 0x9c, 0x0b, 0x7d, 0x45 }
};

// ---------------------------------------------------------------------------
// PrintHex — dump Len bytes at Buf as "XX XX XX ..." groups of 16
// ---------------------------------------------------------------------------
STATIC VOID
PrintHex (
  IN CONST UINT8  *Buf,
  IN UINTN         Len
  )
{
  UINTN  i;
  CHAR16 Line[64];
  UINTN  LinePos;

  LinePos = 0;
  for (i = 0; i < Len; i++) {
    UnicodeSPrint (Line + LinePos, sizeof (Line) - LinePos * sizeof (CHAR16),
                  L"%02x ", Buf[i]);
    LinePos += 3;
    if (LinePos >= 48 || i == Len - 1) {
      Line[LinePos] = L'\0';
      Print (L"  %s\n", Line);
      LinePos = 0;
    }
  }
}

// ---------------------------------------------------------------------------
// WalkHobs — iterate entire HOB list, dump GUID-extension HOBs.
// Returns TRUE if mUefiInfoBlkGuid was found.
// ---------------------------------------------------------------------------
STATIC BOOLEAN
WalkHobs (VOID)
{
  EFI_PEI_HOB_POINTERS  Hob;
  BOOLEAN               Found = FALSE;
  UINTN                 HobIndex = 0;

  Hob.Raw = GetHobList ();
  if (Hob.Raw == NULL) {
    Print (L"[ProbeA] GetHobList() returned NULL\n");
    return FALSE;
  }

  Print (L"[ProbeA] HOB list @ 0x%016lx\n", (UINT64)(UINTN)Hob.Raw);

  while (!END_OF_HOB_LIST (Hob)) {
    UINT16  Type   = Hob.Header->HobType;
    UINT16  Length = Hob.Header->HobLength;

    if (Type == EFI_HOB_TYPE_GUID_EXTENSION) {
      EFI_GUID  *G    = &Hob.Guid->Name;
      UINT8     *Data = (UINT8 *)GET_GUID_HOB_DATA (Hob.Raw);
      UINT16     DataSize = GET_GUID_HOB_DATA_SIZE (Hob.Raw);

      Print (L"[ProbeA] HOB[%03u] GUID_EXT len=%u "
             L"GUID=%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
             HobIndex, Length,
             G->Data1, G->Data2, G->Data3,
             G->Data4[0], G->Data4[1],
             G->Data4[2], G->Data4[3],
             G->Data4[4], G->Data4[5],
             G->Data4[6], G->Data4[7]);

      if (CompareGuid (G, &mUefiInfoBlkGuid)) {
        UINTN DumpLen = (DataSize < 64) ? DataSize : 64;
        Print (L"[ProbeA] *** UefiInfoBlk HOB FOUND — data_size=%u, "
               L"dumping first %u bytes:\n", DataSize, DumpLen);
        PrintHex (Data, DumpLen);

        // Also print raw pointer-width words for easy struct analysis
        {
          UINT64  *Words = (UINT64 *)Data;
          UINTN    WCount = DumpLen / 8;
          UINTN    w;
          Print (L"[ProbeA] UefiInfoBlk qwords:\n");
          for (w = 0; w < WCount; w++) {
            Print (L"  [%02u @ +0x%02x] 0x%016lx\n",
                   w, (UINT32)(w * 8), Words[w]);
          }
        }
        Found = TRUE;
      }
    }

    HobIndex++;
    Hob.Raw = GET_NEXT_HOB (Hob);
  }

  Print (L"[ProbeA] HOB walk done: %u HOBs total\n", HobIndex);
  return Found;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
EFI_STATUS
EFIAPI
UartProbeAEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT64   Tick;
  BOOLEAN  UefiInfoFound;

  // Plant magic — Q5 anchor.  Must appear in UART log.
  Tick = GetPerformanceCounter ();
  Print (L"\n");
  Print (L"=== " PROBE_A_MAGIC L" tick=0x%016lx ===\n", Tick);
  Print (L"[ProbeA] UartProbeA start\n");

  // Q1: HOB walk
  UefiInfoFound = WalkHobs ();

  if (!UefiInfoFound) {
    Print (L"[ProbeA] UefiInfoBlk HOB NOT found in this HOB list\n");
    Print (L"[ProbeA] (Search all GUID_EXT entries above for candidate)\n");
  }

  // Second magic to bracket the output for Q5
  Tick = GetPerformanceCounter ();
  Print (L"=== " PROBE_A_MAGIC L"_END tick=0x%016lx ===\n", Tick);
  Print (L"[ProbeA] done — stall 3s then return\n");

  gBS->Stall (3 * 1000 * 1000);

  return EFI_SUCCESS;
}
