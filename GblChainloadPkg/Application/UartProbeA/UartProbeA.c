/** @file
  UartProbeA v6 — UART buffer WRITE test.

  v5 confirmed: gEfiInfoBlkHobGuid 90A49AFD-... at HOB[004], body's first
  8 bytes = UefiInfoBlk* (= 0xC6E01000 on canoe). 'IBlk' signature
  matched at offset 0; UartLogBufferPtr at +0x38 = 0x81CE4000; len at
  +0x40 = 0x10000 (64 KiB).

  v6 tests whether we can WRITE to 0x81CE4000 from our stage-2 EFI app
  and have the content land in \UefiLog<N>.txt at the next BDS flush.
  If yes, the minimal-logging-design spec's GblSerialPortLib (which
  direct-writes to this buffer) is unblocked. If no, we fall back to
  a lossy null DebugLib for the silent path.

  Strategy:
    1. Walk UefiInfoBlk HOB, read UartLogBufferPtr/Len.
    2. Scan buffer from start for first NUL byte (current end-of-content
       per the SioPortLib zero-init invariant — confirmed in BSP).
    3. Write a unique magic string at that offset.
    4. Exit. User reboots and pulls UefiLog<N>.txt.
    5. Look for the magic — if present, writability confirmed.

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

STATIC CONST EFI_GUID mInfoBlkHobGuid = {
  0x90a49afd, 0x422f, 0x08ae,
  { 0x96, 0x11, 0xe7, 0x88, 0xd3, 0x80, 0x48, 0x45 }
};

#define IBLK_SIGNATURE      0x6B6C4249u
#define UART_PTR_OFFSET     0x38
#define UART_LEN_OFFSET     0x40
#define MAGIC_PREFIX        "GBL_UART_WRITE_PROBE_"

#define CAPTURE_CAP  0x4000
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

EFI_STATUS
EFIAPI
UartProbeAEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_PEI_HOB_POINTERS  Hob;
  EFI_STATUS            Status;
  UINTN                 InfoBlkAddr = 0;
  UINT8                *UartBuf = NULL;
  UINTN                 UartLen = 0;
  UINTN                 EndOfContent = 0;
  UINT64                Tsc;
  CHAR8                 Magic[128];
  UINTN                 MagicLen;

  Tsc = GetPerformanceCounter ();
  Print (L"\n=== UartProbeA v6 (UART buffer WRITE test) tsc=0x%016lx ===\n", Tsc);

  Status = LogFsInit ();
  if (EFI_ERROR (Status)) {
    Print (L"[ProbeA6] LogFsInit failed: %r — screen-only\n", Status);
  }

  CapPrintf ("UartProbeA v6 — UART buffer WRITE test\n");
  CapPrintf ("tsc=0x%016lx\n\n", Tsc);

  /* 1. Find UefiInfoBlk via HOB. */
  Hob.Raw = GetHobList ();
  if (Hob.Raw == NULL) {
    Print (L"[ProbeA6] GetHobList NULL\n");
    CapPrintf ("ERROR: GetHobList NULL\n");
    goto write_out;
  }
  while (!END_OF_HOB_LIST (Hob)) {
    if (Hob.Header->HobType == EFI_HOB_TYPE_GUID_EXTENSION &&
        CompareGuid (&Hob.Guid->Name, &mInfoBlkHobGuid)) {
      UINT8 *Body = (UINT8 *)GET_GUID_HOB_DATA (Hob.Raw);
      InfoBlkAddr = *(UINTN *)Body;
      break;
    }
    Hob.Raw = GET_NEXT_HOB (Hob);
  }
  if (InfoBlkAddr == 0) {
    Print (L"[ProbeA6] UefiInfoBlk HOB not found\n");
    CapPrintf ("ERROR: UefiInfoBlk HOB not found\n");
    goto write_out;
  }
  CapPrintf ("UefiInfoBlk @ 0x%016lx\n", (UINT64)InfoBlkAddr);

  /* 2. Validate signature + read UART fields. */
  {
    UINT8 *S = (UINT8 *)InfoBlkAddr;
    UINT32 Sig = *(UINT32 *)(S + 0);
    if (Sig != IBLK_SIGNATURE) {
      Print (L"[ProbeA6] Sig mismatch: 0x%08x\n", Sig);
      CapPrintf ("ERROR: sig mismatch 0x%08x\n", Sig);
      goto write_out;
    }
    UartBuf = (UINT8 *)(UINTN)(*(UINT64 *)(S + UART_PTR_OFFSET));
    UartLen = (UINTN)(*(UINT64 *)(S + UART_LEN_OFFSET));
    CapPrintf ("UartLogBufferPtr=0x%016lx UartLogBufferLen=0x%lx\n",
               (UINT64)(UINTN)UartBuf, (UINT64)UartLen);
  }

  if (UartBuf == NULL || UartLen < 128) {
    Print (L"[ProbeA6] UART buf NULL or too small\n");
    CapPrintf ("ERROR: UART buf NULL or too small\n");
    goto write_out;
  }

  /* 3. Find current end-of-content via scan-for-NUL. */
  for (EndOfContent = 0; EndOfContent < UartLen; EndOfContent++) {
    if (UartBuf[EndOfContent] == 0) break;
  }
  CapPrintf ("End-of-content (first NUL): offset 0x%lx (%lu bytes of content)\n",
             (UINT64)EndOfContent, (UINT64)EndOfContent);
  if (EndOfContent == UartLen) {
    CapPrintf ("WARN: buffer is full / wrapped — no NUL found; using midpoint\n");
    EndOfContent = UartLen / 2;
  }

  /* 4. Compose magic. */
  MagicLen = AsciiSPrint (Magic, sizeof (Magic),
                           MAGIC_PREFIX "%016lx_END\n", Tsc);
  if (MagicLen + EndOfContent >= UartLen) {
    CapPrintf ("ERROR: no room for magic (need %lu bytes past offset 0x%lx)\n",
               (UINT64)MagicLen, (UINT64)EndOfContent);
    goto write_out;
  }

  /* 5. WRITE the magic into the buffer. THIS is the load-bearing test. */
  CapPrintf ("\nAttempting write of %lu bytes at offset 0x%lx (target addr 0x%016lx):\n",
             (UINT64)MagicLen, (UINT64)EndOfContent,
             (UINT64)(UINTN)(UartBuf + EndOfContent));
  CapPrintf ("Magic: %a", Magic);

  /* Byte-by-byte copy so the access pattern is unambiguous. */
  for (UINTN i = 0; i < MagicLen; i++) {
    UartBuf[EndOfContent + i] = (UINT8)Magic[i];
  }

  CapPrintf ("\nWrite returned without fault. Reading back first 32 bytes:\n");
  for (UINTN i = 0; i < 32 && i < MagicLen; i++) {
    CapPrintf (" %02x", UartBuf[EndOfContent + i]);
  }
  CapPrintf ("\n");

  Print (L"[ProbeA6] Wrote magic at 0x%016lx; reboot + check UefiLog<N>.txt\n",
         (UINT64)(UINTN)(UartBuf + EndOfContent));

write_out:
  if (LogFsIsReady ()) {
    Status = LogFsWriteBlob (L"\\UartProbeA6_WriteTest.txt", mCapture, mCaptureUsed);
    if (EFI_ERROR (Status)) {
      Print (L"[ProbeA6] LogFsWriteBlob failed: %r\n", Status);
    } else {
      Print (L"[ProbeA6] Wrote %u bytes to \\UartProbeA6_WriteTest.txt\n",
             (UINT32)mCaptureUsed);
    }
    LogFsFlush ();
    LogFsClose ();
  }

  Print (L"\n[ProbeA6] done — stalling 5s\n");
  gBS->Stall (5 * 1000 * 1000);
  return EFI_SUCCESS;
}
