/** @file Entry.c — gbl-chainload entry point.
    Single dispatcher: GBL_MODE selects the mode (0 or 1); AUTO/DEBUG/VERBOSE
    are orthogonal flags. **/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>
#include <Library/GblLog.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/PrintLib.h>
#include <Library/DeviceInfo.h>
#include <Library/PartitionTableUpdate.h>
#include <Library/BootESP.h>
#include <Library/LoadFVLib.h>
#include <Library/LogFsLib.h>
#include <Library/Recovery.h>
#include <Library/ShutdownServices.h>

EFI_STATUS FastbootInitialize (VOID);
EFI_STATUS EFIAPI BootFlowChainLoad (VOID);

#ifndef GBL_MODE
# error "GBL_MODE (0 or 1) must be defined at build time"
#endif
#ifndef GBL_AUTO
# define GBL_AUTO 0
#endif
#ifndef GBL_DEBUG
# define GBL_DEBUG 0
#endif
#ifndef GBL_VERBOSE
# define GBL_VERBOSE 0
#endif

#ifndef GBL_CHAINLOAD_VERSION
# define GBL_CHAINLOAD_VERSION "v2"
#endif

#define KEY_WINDOW_MS  3000

/*
 * Screen output is gated by GBL_DEBUG.
 * logfs/EFI_DEBUG always emit regardless of this flag.
 */
#if (GBL_DEBUG == 1)
# define SCR_PRINT(...)  Print (__VA_ARGS__)
#else
# define SCR_PRINT(...)  do {} while (0)
#endif

typedef enum { GblKeyNone, GblKeyVolDown, GblKeyVolUp } GBL_KEY_ACTION;

STATIC GBL_KEY_ACTION
WaitForBootInterrupt (
  IN UINT32 TimeoutMs
  )
{
  EFI_STATUS     Status;
  EFI_EVENT      TimerEvent;
  EFI_EVENT      WaitList[2];
  UINTN          EventIndex;
  EFI_INPUT_KEY  Key;
  GBL_KEY_ACTION KeyDetected = GblKeyNone;

  if (gST == NULL || gST->ConIn == NULL) {
    return GblKeyNone;
  }

  Status = gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, NULL, NULL,
                             &TimerEvent);
  if (EFI_ERROR (Status)) {
    return GblKeyNone;
  }

  Status = gBS->SetTimer (TimerEvent, TimerRelative,
                          (UINT64)TimeoutMs * 10000);
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (TimerEvent);
    return GblKeyNone;
  }

  WaitList[0] = gST->ConIn->WaitForKey;
  WaitList[1] = TimerEvent;

  while (TRUE) {
    Status = gBS->WaitForEvent (2, WaitList, &EventIndex);
    if (EFI_ERROR (Status)) {
      break;
    }
    if (EventIndex == 0) {
      Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
      if (!EFI_ERROR (Status)) {
        if (Key.ScanCode == SCAN_DOWN) {
          KeyDetected = GblKeyVolDown;
          break;
        }
        if (Key.ScanCode == SCAN_UP) {
          KeyDetected = GblKeyVolUp;
          break;
        }
      }
    } else {
      break;
    }
  }

  gBS->CloseEvent (TimerEvent);
  return KeyDetected;
}

STATIC VOID
CommonEarlyInit (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_STATUS Status;

  GBL_INFO ("gbl-chainload | mode=%d auto=%d debug=%d verbose=%d\n",
            (int)GBL_MODE, (int)GBL_AUTO, (int)GBL_DEBUG, (int)GBL_VERBOSE);

  DeviceInfoInit ();
  EnumeratePartitions ();
  UpdatePartitionEntries ();
  SignalSDDetection ();
  LoadDriversFromCurrentFv (ImageHandle);

  Status = LogFsInit ();
  if (Status == EFI_NOT_FOUND) {
    /* Always show fatal/recovery messages even with GBL_DEBUG=0. */
    Print (L"!!! LOGFS PARTITION NOT FOUND - LOGGING TO CONSOLE ONLY !!!\n");
  }
}

STATIC VOID
EnterFastboot (VOID)
{
  EFI_STATUS Status;

  SCR_PRINT (L"gbl-chainload: entering FastbootLib\n");
  LogFsClose ();

  Status = FastbootInitialize ();
  if (EFI_ERROR (Status)) {
    /* Fatal: always show even in DEBUG=0. */
    Print (L"FastbootInitialize returned %r — dead-end\n", Status);
  }
}

STATIC VOID
TryChainLoad (VOID)
{
  EFI_STATUS Status;

  SCR_PRINT (L"gbl-chainload: chain-loading patched ABL\n");

  Status = BootFlowChainLoad ();

  /* On return, BootFlow already logged. Fall through to fastboot as recovery. */
  SCR_PRINT (L"gbl-chainload: BootFlow returned %r — falling to fastboot\n",
             Status);
}

EFI_STATUS
EFIAPI
GblChainloadEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  GBL_KEY_ACTION Key;

  /* Banner: always to logfs/DEBUG, screen only under GBL_DEBUG=1. */
  SCR_PRINT (L"\ngbl-chainload %a — mode=%d auto=%d debug=%d verbose=%d"
             L" (%a %a)\n",
             GBL_CHAINLOAD_VERSION,
             (int)GBL_MODE, (int)GBL_AUTO,
             (int)GBL_DEBUG, (int)GBL_VERBOSE,
             __DATE__, __TIME__);

  CommonEarlyInit (ImageHandle);

#if (GBL_AUTO == 0)
  SCR_PRINT (L"Hold VolUp within %us to enter FastbootLib; "
             L"timeout chain-loads silently.\n",
             KEY_WINDOW_MS / 1000);
#else
  SCR_PRINT (L"Hold VolUp within %us to chain-load patched ABL immediately; "
             L"timeout enters FastbootLib (await `oem escape`).\n",
             KEY_WINDOW_MS / 1000);
#endif

  Key = WaitForBootInterrupt (KEY_WINDOW_MS);

#if (GBL_AUTO == 0)
  /*
   * Production default: timeout chain-loads silently.
   * VolUp inverts (enter fastboot for host-driven control).
   * VolDown is a placeholder — same default path.
   */
  if (Key == GblKeyVolUp) {
    SCR_PRINT (L"VolUp escape: entering FastbootLib\n");
    EnterFastboot ();
  } else {
    TryChainLoad ();
    /*
     * If chain-load returns (shouldn't happen on success), fall through
     * to FastbootLib as recovery surface.
     */
    EnterFastboot ();
  }
#else
  /*
   * Host-gated (GBL_AUTO=1): timeout enters fastboot, awaiting `oem escape`.
   * VolUp forces immediate chain-load without waiting for host.
   */
  if (Key == GblKeyVolUp) {
    SCR_PRINT (L"VolUp escape: chain-loading patched ABL\n");
    TryChainLoad ();
    /* If chainload returns, fall through to fastboot. */
  }
  EnterFastboot ();
#endif

  /* Spin forever — fastboot enters its own event loop, but if it returns... */
  while (TRUE) {
    gBS->Stall (1000000);
  }

  return EFI_SUCCESS;
}
