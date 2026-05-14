/** @file
  UartProbeA v4 — validated-deref of pointer candidates from v3 inventory.

  v3 captured 9 HOBs with 8-byte bodies that look like potential pointers
  to a UefiInfoBlk struct. None had the 'IBlk' signature at the HOB body
  itself (so option (a) "body-IS-struct" is dead). Now we test each
  candidate as a pointer — but ONLY if EFI memory map confirms the
  pointed-to range is in mapped DRAM (boot/runtime/conventional). Reading
  from unmapped MMIO regions is what crashed the display driver on v2.

  For each candidate that passes the memory-map gate, we read 64 bytes at
  the pointed-to address and check for the IBlk signature at offset 0.

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

#define IBLK_SIGNATURE  0x6B6C4249u

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

/* Allocate memory-map buffer, return it. Caller must FreePool. */
STATIC EFI_MEMORY_DESCRIPTOR *
GetMemMap (
  OUT UINTN *MapSizeOut,
  OUT UINTN *DescSizeOut
  )
{
  EFI_STATUS              Status;
  EFI_MEMORY_DESCRIPTOR  *Map = NULL;
  UINTN                   MapSize = 0;
  UINTN                   MapKey;
  UINTN                   DescSize;
  UINT32                  DescVer;

  /* First call returns BUFFER_TOO_SMALL with required size. */
  Status = gBS->GetMemoryMap (&MapSize, NULL, &MapKey, &DescSize, &DescVer);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    CapPrintf ("GetMemoryMap sizing call returned %r — abort\n", Status);
    return NULL;
  }

  /* Allocate generously (map can grow between calls). */
  MapSize += 4 * DescSize;
  Map = AllocatePool (MapSize);
  if (Map == NULL) {
    CapPrintf ("AllocatePool(%u) failed\n", (UINT32)MapSize);
    return NULL;
  }

  Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVer);
  if (EFI_ERROR (Status)) {
    CapPrintf ("GetMemoryMap returned %r\n", Status);
    FreePool (Map);
    return NULL;
  }

  *MapSizeOut  = MapSize;
  *DescSizeOut = DescSize;
  return Map;
}

/* Returns TRUE if Addr is inside a mapped region of a kind we consider safe
 * to read from. Conservative: only ConventionalMemory / BootServicesData /
 * RuntimeServicesData / LoaderData / BootServicesCode / LoaderCode. */
STATIC BOOLEAN
IsSafeToReadAddr (
  IN  EFI_MEMORY_DESCRIPTOR  *Map,
  IN  UINTN                   MapSize,
  IN  UINTN                   DescSize,
  IN  UINTN                   Addr,
  OUT EFI_MEMORY_TYPE        *TypeOut
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
    if (Addr >= Start && Addr + 64 <= EndExcl) {
      *TypeOut = d->Type;
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
  *TypeOut = (EFI_MEMORY_TYPE)0xFFFFFFFF;
  return FALSE;
}

STATIC CONST CHAR8 *
MemTypeName (
  IN EFI_MEMORY_TYPE T
  )
{
  switch (T) {
    case EfiReservedMemoryType:      return "Reserved";
    case EfiLoaderCode:              return "LoaderCode";
    case EfiLoaderData:              return "LoaderData";
    case EfiBootServicesCode:        return "BootSvcCode";
    case EfiBootServicesData:        return "BootSvcData";
    case EfiRuntimeServicesCode:     return "RtSvcCode";
    case EfiRuntimeServicesData:     return "RtSvcData";
    case EfiConventionalMemory:      return "ConvMem";
    case EfiUnusableMemory:          return "Unusable";
    case EfiACPIReclaimMemory:       return "ACPIRecl";
    case EfiACPIMemoryNVS:           return "ACPINvs";
    case EfiMemoryMappedIO:          return "MMIO";
    case EfiMemoryMappedIOPortSpace: return "MMIO_Port";
    case EfiPalCode:                 return "PalCode";
    case EfiPersistentMemory:        return "Persistent";
    default:                          return "Unknown/Unmapped";
  }
}

/* Candidate set discovered in v3 run. Hard-coded so we don't need a second
 * HOB walk. Indexed by HOB number in the v3 inventory for cross-reference. */
typedef struct {
  CONST CHAR8 *HobIdLabel;
  UINTN        Address;
} POINTER_CANDIDATE;

STATIC CONST POINTER_CANDIDATE mCandidates[] = {
  { "HOB[011] 8EC2BD8D-...",          0xC683FE98UL },
  { "HOB[015] D5F8D706-...",          0xFFFFFF80300F0301UL },
  { "HOB[017] F725411A-...",          0xFFFFFF8060260211UL },
  { "HOB[085] 14D72BF6-...",          0xC1A90000UL },
  { "HOB[086] C095791A-...",          0x7ECCBFC8UL },
  { "HOB[088] B323179B-...",          0xC683F3F8UL },
  { "HOB[091] 12DBD93D-...",          0xC1A92000UL },
  { "HOB[094] 7B0EEEBF-...",          0xC683F840UL },
  /* HOB[090] (value 0x84A) deliberately omitted — too small to be a pointer. */
};
#define CAND_COUNT  (sizeof (mCandidates) / sizeof (mCandidates[0]))

EFI_STATUS
EFIAPI
UartProbeAEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS              Status;
  EFI_MEMORY_DESCRIPTOR  *Map = NULL;
  UINTN                   MapSize = 0;
  UINTN                   DescSize = 0;
  UINTN                   i;
  UINTN                   IBlkHit = 0;

  Print (L"\n=== UartProbeA v4 (validated-deref) tsc=0x%016lx ===\n",
         GetPerformanceCounter ());

  /* Mount logfs first so we have somewhere durable to write findings. */
  Status = LogFsInit ();
  if (EFI_ERROR (Status)) {
    Print (L"[ProbeA4] LogFsInit failed: %r — screen-only fallback\n", Status);
  } else {
    Print (L"[ProbeA4] LogFs mounted\n");
  }

  CapPrintf ("UartProbeA v4 — pointer-candidate deref\n");
  CapPrintf ("IBLK_SIGNATURE=0x%08x\n\n", IBLK_SIGNATURE);

  /* Get memory map. */
  Map = GetMemMap (&MapSize, &DescSize);
  if (Map == NULL) {
    Print (L"[ProbeA4] GetMemoryMap failed — abort\n");
    CapPrintf ("ERROR: GetMemoryMap failed\n");
    goto write_out;
  }
  CapPrintf ("Memory map: size=%u desc_size=%u (%u descriptors)\n\n",
             (UINT32)MapSize, (UINT32)DescSize, (UINT32)(MapSize / DescSize));

  Print (L"[ProbeA4] Memory map: %u descriptors\n", (UINT32)(MapSize / DescSize));

  /* Test each candidate. */
  for (i = 0; i < CAND_COUNT; i++) {
    UINTN              Addr  = mCandidates[i].Address;
    CONST CHAR8       *Label = mCandidates[i].HobIdLabel;
    EFI_MEMORY_TYPE    MemType;
    BOOLEAN            Safe;

    CapPrintf ("--- candidate %u: %a addr=0x%016lx\n",
               (UINT32)i, Label, (UINT64)Addr);
    Print (L"[ProbeA4] cand %u: 0x%016lx ", (UINT32)i, (UINT64)Addr);

    Safe = IsSafeToReadAddr (Map, MapSize, DescSize, Addr, &MemType);
    CapPrintf ("  memory-map: type=%a safe=%a\n",
               MemTypeName (MemType), Safe ? "YES" : "NO");
    Print (L"%a (%a)\n", MemTypeName (MemType), Safe ? "OK" : "SKIP");

    if (!Safe) {
      CapPrintf ("  SKIPPED — not in safe-to-read region\n\n");
      continue;
    }

    /* Read first 64 bytes and check signature. Dereference is now guarded
     * by the memory map check above — we know this 64-byte window is in
     * a mapped region of a type that's safe to read. */
    {
      UINT8 *Target = (UINT8 *)Addr;
      UINT32 Sig = *(UINT32 *)Target;

      CapPrintf ("  first 4 bytes: %02x %02x %02x %02x  (as UINT32 LE: 0x%08x)\n",
                 Target[0], Target[1], Target[2], Target[3], Sig);
      CapPrintf ("  first 64 bytes at target:");
      CapHex (Target, 64);

      if (Sig == IBLK_SIGNATURE) {
        CapPrintf ("\n  *** IBLK SIGNATURE MATCH — UefiInfoBlk found here ***\n");
        CapPrintf ("  Reading more (256 bytes) for struct analysis:\n");
        CapHex (Target, 256);

        /* Print 8-byte words at common offsets where UART buffer ptr/len
         * may be. SM8350 BSP has UartLogBufferPtr at +0x38, len at +0x40.
         * Show enough surrounding context to identify by hand if offsets
         * differ on SM8550. */
        CapPrintf ("\n  Field hints (BSP convention — verify by hand):\n");
        CapPrintf ("    +0x00 Signature:                    0x%08x\n",
                   *(UINT32 *)(Target + 0));
        CapPrintf ("    +0x04 StructVersion:                0x%08x\n",
                   *(UINT32 *)(Target + 4));
        CapPrintf ("    +0x08 RelInfo (CHAR8*):             0x%016lx\n",
                   *(UINT64 *)(Target + 8));
        CapPrintf ("    +0x10 FdMemBase:                    0x%016lx\n",
                   *(UINT64 *)(Target + 0x10));
        CapPrintf ("    +0x18 DDRMemSize:                   0x%016lx\n",
                   *(UINT64 *)(Target + 0x18));
        CapPrintf ("    +0x20 StackBase:                    0x%016lx\n",
                   *(UINT64 *)(Target + 0x20));
        CapPrintf ("    +0x28 StackSize:                    0x%016lx\n",
                   *(UINT64 *)(Target + 0x28));
        CapPrintf ("    +0x30 HobBase:                      0x%016lx\n",
                   *(UINT64 *)(Target + 0x30));
        CapPrintf ("    +0x38 UartLogBufferPtr (BSP guess): 0x%016lx\n",
                   *(UINT64 *)(Target + 0x38));
        CapPrintf ("    +0x40 UartLogBufferLen (BSP guess): 0x%016lx\n",
                   *(UINT64 *)(Target + 0x40));
        IBlkHit++;
      } else {
        CapPrintf ("  no IBlk signature here\n");
      }
    }
    CapPrintf ("\n");
  }

  CapPrintf ("=== summary ===\n");
  CapPrintf ("Candidates tested:     %u\n", (UINT32)CAND_COUNT);
  CapPrintf ("IBlk-signature hits:   %u\n", (UINT32)IBlkHit);

  Print (L"[ProbeA4] %u/%u candidates tested, %u IBlk-signature hits\n",
         (UINT32)CAND_COUNT, (UINT32)CAND_COUNT, (UINT32)IBlkHit);

  FreePool (Map);

write_out:
  if (LogFsIsReady ()) {
    Status = LogFsWriteBlob (L"\\UartProbeA4_Deref.txt",
                             mCapture, mCaptureUsed);
    if (EFI_ERROR (Status)) {
      Print (L"[ProbeA4] LogFsWriteBlob failed: %r\n", Status);
    } else {
      Print (L"[ProbeA4] Wrote %u bytes to \\UartProbeA4_Deref.txt\n",
             (UINT32)mCaptureUsed);
    }
    LogFsFlush ();
    LogFsClose ();
  } else {
    Print (L"[ProbeA4] LogFs not ready — captured %u bytes; first 256:\n",
           (UINT32)mCaptureUsed);
    for (UINTN k = 0; k < mCaptureUsed && k < 256; k++) {
      CHAR16  C[2] = { (CHAR16)mCapture[k], 0 };
      if (mCapture[k] < 0x20 && mCapture[k] != '\n') C[0] = '.';
      Print (L"%s", C);
    }
    Print (L"\n[ProbeA4] (truncated)\n");
  }

  Print (L"\n[ProbeA4] done — stalling 5s\n");
  gBS->Stall (5 * 1000 * 1000);
  return EFI_SUCCESS;
}
