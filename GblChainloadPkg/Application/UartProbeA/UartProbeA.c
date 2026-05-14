/** @file
  UartProbeA v5 — final probe. Targets gEfiInfoBlkHobGuid by exact GUID
  (literal 90A49AFD-422F-08AE-9611-E788D3804845 per QcomPkg.dec), reads
  first 8 bytes of the HOB body as the UefiInfoBlk pointer, dereferences
  it (after EFI memory-map validation), confirms 'IBlk' signature, and
  dumps the struct + UART log buffer pointer/length.

  Per the BSP consumer pattern in QcomBaseLib.c::GetInfoBlock():
    GuidHob = GetFirstGuidHob(&gEfiInfoBlkHobGuid);
    DataPtr = GET_GUID_HOB_DATA(GuidHob);       // UINTN**
    return (VOID*) *DataPtr;                    // UefiInfoBlk*

  On canoe (BOOT.MXF.2.5.3-00131-KAANAPALI), HOB[004]'s body was 32 bytes;
  the first 8 bytes contain the UefiInfoBlk pointer (0xC6E01000 per v3
  inventory) and the remaining 24 bytes are unused or version-specific
  padding.

  Output written to \UartProbeA5_UefiInfoBlk.txt on logfs.

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
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/HobLib.h>
#include <Library/TimerLib.h>
#include <Library/LogFsLib.h>

/* gEfiInfoBlkHobGuid literal from ~/BOOT.MXF.2.5.1/.../QcomPkg/QcomPkg.dec */
STATIC CONST EFI_GUID mInfoBlkHobGuid = {
  0x90a49afd, 0x422f, 0x08ae,
  { 0x96, 0x11, 0xe7, 0x88, 0xd3, 0x80, 0x48, 0x45 }
};

#define IBLK_SIGNATURE  0x6B6C4249u   /* 'I'|'B'<<8|'l'<<16|'k'<<24 */

#define CAPTURE_CAP  0x10000
STATIC CHAR8 mCapture[CAPTURE_CAP];
STATIC UINTN mCaptureUsed = 0;

STATIC VOID EFIAPI
CapPrintf (
  IN CONST CHAR8 *Fmt,
  ...
  )
{
  VA_LIST Marker;
  UINTN   Avail, Wrote;

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

/* Returns TRUE if Addr..Addr+Len is inside a mapped safe-to-read region. */
STATIC BOOLEAN
IsSafeRange (
  IN EFI_MEMORY_DESCRIPTOR *Map,
  IN UINTN                  MapSize,
  IN UINTN                  DescSize,
  IN UINTN                  Addr,
  IN UINTN                  Len
  )
{
  UINT8                  *p   = (UINT8 *)Map;
  UINT8                  *end = p + MapSize;
  EFI_MEMORY_DESCRIPTOR  *d;
  UINTN                   Start, EndExcl;

  while (p < end) {
    d = (EFI_MEMORY_DESCRIPTOR *)p;
    Start   = (UINTN)d->PhysicalStart;
    EndExcl = Start + (UINTN)d->NumberOfPages * EFI_PAGE_SIZE;
    if (Addr >= Start && Addr + Len <= EndExcl) {
      switch (d->Type) {
        case EfiLoaderCode:
        case EfiLoaderData:
        case EfiBootServicesCode:
        case EfiBootServicesData:
        case EfiRuntimeServicesCode:
        case EfiRuntimeServicesData:
        case EfiConventionalMemory:
        case EfiACPIReclaimMemory:
        case EfiACPIMemoryNVS:
          return TRUE;
        default:
          return FALSE;
      }
    }
    p += DescSize;
  }
  return FALSE;
}

EFI_STATUS
EFIAPI
UartProbeAEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS              Status;
  EFI_PEI_HOB_POINTERS    Hob;
  UINTN                   InfoBlkPtrVal = 0;
  EFI_MEMORY_DESCRIPTOR  *Map = NULL;
  UINTN                   MapSize = 0;
  UINTN                   MapKey;
  UINTN                   DescSize = 0;
  UINT32                  DescVer;

  Print (L"\n=== UartProbeA v5 (final) tsc=0x%016lx ===\n",
         GetPerformanceCounter ());

  Status = LogFsInit ();
  if (EFI_ERROR (Status)) {
    Print (L"[ProbeA5] LogFsInit failed: %r — screen-only fallback\n", Status);
  } else {
    Print (L"[ProbeA5] LogFs mounted\n");
  }

  CapPrintf ("UartProbeA v5 — UefiInfoBlk via gEfiInfoBlkHobGuid\n");
  CapPrintf ("Target GUID: 90A49AFD-422F-08AE-9611-E788D3804845\n");
  CapPrintf ("Expected: HOB body's first 8 bytes = UefiInfoBlk*, target has 'IBlk' sig.\n\n");

  /* Step 1: find HOB by GUID. */
  Hob.Raw = GetHobList ();
  if (Hob.Raw == NULL) {
    CapPrintf ("ERROR: GetHobList returned NULL\n");
    Print (L"[ProbeA5] GetHobList NULL\n");
    goto write_out;
  }
  while (!END_OF_HOB_LIST (Hob)) {
    if (Hob.Header->HobType == EFI_HOB_TYPE_GUID_EXTENSION) {
      if (CompareGuid (&Hob.Guid->Name, &mInfoBlkHobGuid)) {
        UINT8  *Body     = (UINT8 *)GET_GUID_HOB_DATA (Hob.Raw);
        UINT16  DataSize = GET_GUID_HOB_DATA_SIZE (Hob.Raw);

        CapPrintf ("HOB matched: data_size=%u\n", (UINT32)DataSize);
        CapPrintf ("Body bytes:");
        CapHex (Body, DataSize);

        if (DataSize >= sizeof (UINTN)) {
          InfoBlkPtrVal = *(UINTN *)Body;
          CapPrintf ("\nFirst 8 bytes as UINTN: 0x%016lx\n", (UINT64)InfoBlkPtrVal);
          Print (L"[ProbeA5] InfoBlkPtr from HOB = 0x%016lx\n",
                 (UINT64)InfoBlkPtrVal);
        } else {
          CapPrintf ("\nERROR: body too small for a UINTN pointer\n");
        }
        break;
      }
    }
    Hob.Raw = GET_NEXT_HOB (Hob);
  }

  if (InfoBlkPtrVal == 0) {
    CapPrintf ("\nERROR: gEfiInfoBlkHobGuid HOB not found or pointer was zero\n");
    Print (L"[ProbeA5] HOB not found or zero pointer\n");
    goto write_out;
  }

  /* Step 2: validate the pointer via EFI memory map. */
  Status = gBS->GetMemoryMap (&MapSize, NULL, &MapKey, &DescSize, &DescVer);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    CapPrintf ("GetMemoryMap sizing call failed: %r\n", Status);
    goto write_out;
  }
  MapSize += 4 * DescSize;
  Map = AllocatePool (MapSize);
  if (Map == NULL) {
    CapPrintf ("AllocatePool failed\n");
    goto write_out;
  }
  Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVer);
  if (EFI_ERROR (Status)) {
    CapPrintf ("GetMemoryMap failed: %r\n", Status);
    goto write_out;
  }

  if (!IsSafeRange (Map, MapSize, DescSize, InfoBlkPtrVal, 256)) {
    CapPrintf ("\nERROR: InfoBlkPtr 0x%016lx is NOT in a safe-to-read region\n",
               (UINT64)InfoBlkPtrVal);
    Print (L"[ProbeA5] InfoBlkPtr not in safe region — abort\n");
    goto write_out;
  }
  CapPrintf ("\nInfoBlkPtr 0x%016lx is in a safe-to-read EFI memory region\n",
             (UINT64)InfoBlkPtrVal);

  /* Step 3: dump struct + identify fields. */
  {
    UINT8  *S   = (UINT8 *)InfoBlkPtrVal;
    UINT32  Sig = *(UINT32 *)(S + 0);

    CapPrintf ("\n--- UefiInfoBlk @ 0x%016lx (first 256 bytes) ---",
               (UINT64)InfoBlkPtrVal);
    CapHex (S, 256);

    CapPrintf ("\n--- field reads (BSP convention) ---\n");
    CapPrintf ("  +0x00 Signature:                    0x%08x %a\n",
               Sig, Sig == IBLK_SIGNATURE ? "(== 'IBlk' — MATCH)" : "(no match)");
    CapPrintf ("  +0x04 StructVersion:                0x%08x\n",
               *(UINT32 *)(S + 4));
    CapPrintf ("  +0x08 RelInfo (CHAR8*):             0x%016lx\n",
               *(UINT64 *)(S + 8));
    CapPrintf ("  +0x10 FdMemBase:                    0x%016lx\n",
               *(UINT64 *)(S + 0x10));
    CapPrintf ("  +0x18 DDRMemSize:                   0x%016lx\n",
               *(UINT64 *)(S + 0x18));
    CapPrintf ("  +0x20 StackBase:                    0x%016lx\n",
               *(UINT64 *)(S + 0x20));
    CapPrintf ("  +0x28 StackSize:                    0x%016lx\n",
               *(UINT64 *)(S + 0x28));
    CapPrintf ("  +0x30 HobBase:                      0x%016lx\n",
               *(UINT64 *)(S + 0x30));
    CapPrintf ("  +0x38 UartLogBufferPtr (BSP guess): 0x%016lx\n",
               *(UINT64 *)(S + 0x38));
    CapPrintf ("  +0x40 UartLogBufferLen (BSP guess): 0x%016lx\n",
               *(UINT64 *)(S + 0x40));

    if (Sig == IBLK_SIGNATURE) {
      UINT64  UartPtr64 = *(UINT64 *)(S + 0x38);
      UINT64  UartLen64 = *(UINT64 *)(S + 0x40);

      Print (L"[ProbeA5] UefiInfoBlk Sig OK; UartLogBufferPtr=0x%016lx Len=0x%016lx\n",
             UartPtr64, UartLen64);

      /* If UART pointer is also in safe range, peek at content */
      if (UartPtr64 != 0 && IsSafeRange (Map, MapSize, DescSize,
                                          (UINTN)UartPtr64, 128)) {
        UINT8  *Uart = (UINT8 *)(UINTN)UartPtr64;
        CapPrintf ("\n--- UART buffer @ 0x%016lx, first 128 bytes ---",
                   UartPtr64);
        CapHex (Uart, 128);
      } else {
        CapPrintf ("\n(UART buffer pointer not in safe memory map — cannot peek)\n");
      }
    } else {
      Print (L"[ProbeA5] UefiInfoBlk sig MISMATCH (0x%08x) — struct layout differs\n", Sig);
    }
  }

  FreePool (Map);

write_out:
  if (LogFsIsReady ()) {
    Status = LogFsWriteBlob (L"\\UartProbeA5_UefiInfoBlk.txt",
                             mCapture, mCaptureUsed);
    if (EFI_ERROR (Status)) {
      Print (L"[ProbeA5] LogFsWriteBlob failed: %r\n", Status);
    } else {
      Print (L"[ProbeA5] Wrote %u bytes to \\UartProbeA5_UefiInfoBlk.txt\n",
             (UINT32)mCaptureUsed);
    }
    LogFsFlush ();
    LogFsClose ();
  }

  Print (L"\n[ProbeA5] done — stalling 5s\n");
  gBS->Stall (5 * 1000 * 1000);
  return EFI_SUCCESS;
}
