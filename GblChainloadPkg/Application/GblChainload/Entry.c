/** @file Entry.c — gbl-chainload entry point.
    Single dispatcher: GBL_MODE selects the mode (0 or 1); AUTO/DEBUG/VERBOSE
    are orthogonal flags. **/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>
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

#ifndef GBL_CHAINLOAD_VERSION
# define GBL_CHAINLOAD_VERSION "v2"
#endif

#define KEY_WINDOW_MS  3000

/* Screen output policy (single coherent system, see LogFsLib/DebugSink.c):
 *   Print(L"...")              — always shown on screen (and always logged
 *                                via the hook). Use for failures, fatal
 *                                warnings, and user-interrupt acks.
 *   DEBUG((DEBUG_ERROR, "..."))— always shown on screen + always logged.
 *                                Use for boundary markers + error lines
 *                                that carry a %r status code.
 *   DEBUG((DEBUG_INFO,  "..."))— always logged; screen-visible only when
 *                                GBL_DEBUG=1 (LogFsSetScreenMask widens
 *                                the mask in CommonEarlyInit).
 * No SCR_PRINT, no per-callsite #if GBL_DEBUG.
 */

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

  DEBUG ((DEBUG_INFO,
          "gbl-chainload | mode=%d auto=%d debug=%d\n",
          (int)GBL_MODE, (int)GBL_AUTO, (int)GBL_DEBUG));

  DeviceInfoInit ();
  EnumeratePartitions ();
  UpdatePartitionEntries ();
  SignalSDDetection ();
  LoadDriversFromCurrentFv (ImageHandle);

  Status = LogFsInit ();
  if (Status == EFI_NOT_FOUND) {
    /* Always show fatal/recovery messages even with GBL_DEBUG=0. */
    Print (L"!!! LOGFS PARTITION NOT FOUND - LOGGING TO CONSOLE ONLY !!!\n");
  } else if (!EFI_ERROR (Status)) {
    LogFsInstallDebugSink ();
    /* gGblScreenMask intentionally stays at its DEBUG_ERROR default.
     * Widening it would tee DEBUG_INFO output onto UefiLog via the
     * platform's ConOut→UefiLog coupling, diluting UefiLog with
     * gbl-chainload status content. GBL_VERBOSE=1 instead widens
     * PcdDebugPrintErrorLevel to admit GBL_DBG_LOGFS_ONLY — that
     * tier reaches the hook and lands in logfs only. */
    LogFsFlush ();
  }
}

STATIC VOID
EnterFastboot (VOID)
{
  EFI_STATUS Status;

  /* Boundary marker — INFO level: logged to gbl-chainload_BootN.txt
   * always, screen+UefiLog only when --debug widens the mask. Production
   * boots stay silent on entry/exit; correlation with ABL/UefiLog timing
   * needs a --debug build. */
  DEBUG ((DEBUG_INFO, "gbl-chainload exiting (path=fastboot-fallback)\n"));
  DEBUG ((DEBUG_INFO, "gbl-chainload: entering FastbootLib\n"));
  LogFsFlush ();
  LogFsRemoveDebugSink ();
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

  /* Boundary marker — INFO level (see EnterFastboot comment). */
  DEBUG ((DEBUG_INFO, "gbl-chainload exiting (path=chainload)\n"));
  DEBUG ((DEBUG_INFO, "gbl-chainload: chain-loading patched ABL\n"));
  LogFsFlush ();

  Status = BootFlowChainLoad ();

  /* On return, BootFlow already logged the failure. Surface it on the
   * screen too — chainload returning is itself a failure (it should have
   * handed off to the patched ABL). */
  Print (L"gbl-chainload: BootFlow returned %r — falling to fastboot\n",
         Status);
  LogFsFlush ();
}

EFI_STATUS
EFIAPI
GblChainloadEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  GBL_KEY_ACTION Key;

  /* Banner: DEBUG_INFO — always logged, screen-visible only under
   * GBL_DEBUG=1 (via widened mask). ASCII so it lands inside the
   * standard DEBUG() format. */
  DEBUG ((DEBUG_INFO,
          "\ngbl-chainload %a -- mode=%d auto=%d debug=%d (%a %a)\n",
          GBL_CHAINLOAD_VERSION,
          (int)GBL_MODE, (int)GBL_AUTO, (int)GBL_DEBUG,
          __DATE__, __TIME__));

  CommonEarlyInit (ImageHandle);

  /* Boundary marker — INFO level. The chainload entry surface is logged
   * to gbl-chainload_BootN.txt every boot regardless; this DEBUG only
   * reaches the screen and UefiLog under --debug. */
  DEBUG ((DEBUG_INFO, "gbl-chainload entered (mode=%d build=%a)\n",
          (int)GBL_MODE, GBL_BUILD_NAME));

#if (GBL_AUTO == 0)
  DEBUG ((DEBUG_INFO,
          "Hold VolUp within %us to enter FastbootLib; "
          "timeout chain-loads silently.\n",
          KEY_WINDOW_MS / 1000));
#else
  DEBUG ((DEBUG_INFO,
          "Hold VolUp within %us to chain-load patched ABL immediately; "
          "timeout enters FastbootLib (await `oem escape`).\n",
          KEY_WINDOW_MS / 1000));
#endif

  Key = WaitForBootInterrupt (KEY_WINDOW_MS);

#if (GBL_AUTO == 0)
  /*
   * Production default: timeout chain-loads silently.
   * VolUp inverts (enter fastboot for host-driven control).
   * VolDown is a placeholder — same default path.
   */
  if (Key == GblKeyVolUp) {
    /* User interrupt — always visible on screen. */
    Print (L"VolUp escape: entering FastbootLib\n");
    EnterFastboot ();
  } else {
    TryChainLoad ();
    /*
     * If chain-load returns (shouldn't happen on success), drop into
     * FastbootLib as the recovery surface.
     */
    EnterFastboot ();
  }
#else
  /*
   * Host-gated (GBL_AUTO=1): timeout enters fastboot, awaiting `oem escape`.
   * VolUp forces immediate chain-load without waiting for host.
   */
  if (Key == GblKeyVolUp) {
    /* User interrupt — always visible on screen. */
    Print (L"VolUp escape: chain-loading patched ABL\n");
    TryChainLoad ();
    /* If chainload returns, drop into fastboot. */
  }
  EnterFastboot ();
#endif

  /* Spin forever — fastboot enters its own event loop, but if it returns... */
  while (TRUE) {
    gBS->Stall (1000000);
  }

  return EFI_SUCCESS;
}
