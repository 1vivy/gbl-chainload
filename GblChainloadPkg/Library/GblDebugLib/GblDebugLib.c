/** @file GblDebugLib.c — DebugLib shim for gbl-chainload.

  Wraps UefiDebugLibConOut behaviour but records the ErrorLevel in a
  module-scope global (gDbgCurrentLevel) *before* calling ConOut->OutputString.
  LogFsLib's DebugSink (HookedOutputString) reads that global to decide
  whether to suppress the ConOut passthrough: only DEBUG_ERROR lines
  are forwarded to ConOut (→ QCOM BDS → UefiLog); everything else is
  captured only by the gbl-chainload logfs stream.

  Print() calls never go through DebugPrint, so they keep gDbgCurrentLevel
  at its sentinel value (~0) and always pass through to ConOut unchanged —
  the explicit Print() messages in Entry.c and BootFlow.c remain visible
  on both the console and UefiLog.

  Constructor / destructor match UefiDebugLibConOut exactly so the build
  system is satisfied.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/PcdLib.h>
#include <Library/DebugLib.h>
#include <Library/DebugPrintErrorLevelLib.h>

/* Sentinel: not currently inside a DEBUG() call (e.g. a bare Print() path). */
#define GBL_DBG_LEVEL_NONE  ((UINTN)-1)

#define MAX_DEBUG_MESSAGE_LENGTH  0x100

/* Shared with DebugSink.c via extern — same link unit (LogFsLib). */
UINTN   gDbgCurrentLevel = GBL_DBG_LEVEL_NONE;

STATIC BOOLEAN mPostEBS  = FALSE;
STATIC EFI_EVENT mExitBootServicesEvent;
STATIC EFI_SYSTEM_TABLE *mDebugST = NULL;

STATIC VOID EFIAPI
ExitBootServicesCallback (
  EFI_EVENT  Event,
  VOID      *Context
  )
{
  mPostEBS = TRUE;
}

EFI_STATUS
EFIAPI
DxeDebugLibConstructor (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  mDebugST = SystemTable;
  SystemTable->BootServices->CreateEvent (
    EVT_SIGNAL_EXIT_BOOT_SERVICES,
    TPL_NOTIFY,
    ExitBootServicesCallback,
    NULL,
    &mExitBootServicesEvent
    );
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
DxeDebugLibDestructor (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  if (mExitBootServicesEvent != NULL) {
    SystemTable->BootServices->CloseEvent (mExitBootServicesEvent);
  }
  return EFI_SUCCESS;
}

/* Core: format and emit.  Set gDbgCurrentLevel so HookedOutputString knows
   which level triggered this ConOut write. */
STATIC VOID
GblDebugPrintMarker (
  IN UINTN        ErrorLevel,
  IN CONST CHAR8 *Format,
  IN VA_LIST      Marker
  )
{
  CHAR16  Buffer[MAX_DEBUG_MESSAGE_LENGTH];

  if (mPostEBS) {
    return;
  }
  ASSERT (Format != NULL);

  if ((ErrorLevel & GetDebugPrintErrorLevel ()) == 0) {
    return;
  }

  UnicodeVSPrintAsciiFormat (Buffer, sizeof (Buffer), Format, Marker);

  if ((mDebugST != NULL) && (mDebugST->ConOut != NULL)) {
    gDbgCurrentLevel = ErrorLevel;            /* tell HookedOutputString */
    mDebugST->ConOut->OutputString (mDebugST->ConOut, Buffer);
    gDbgCurrentLevel = GBL_DBG_LEVEL_NONE;   /* clear after return      */
  }
}

VOID
EFIAPI
DebugPrint (
  IN UINTN        ErrorLevel,
  IN CONST CHAR8 *Format,
  ...
  )
{
  VA_LIST Marker;

  VA_START (Marker, Format);
  GblDebugPrintMarker (ErrorLevel, Format, Marker);
  VA_END (Marker);
}

VOID
EFIAPI
DebugVPrint (
  IN UINTN        ErrorLevel,
  IN CONST CHAR8 *Format,
  IN VA_LIST      Marker
  )
{
  GblDebugPrintMarker (ErrorLevel, Format, Marker);
}

VOID
EFIAPI
DebugBPrint (
  IN UINTN        ErrorLevel,
  IN CONST CHAR8 *Format,
  IN BASE_LIST    BaseListMarker
  )
{
  CHAR16  Buffer[MAX_DEBUG_MESSAGE_LENGTH];

  if (mPostEBS) {
    return;
  }
  ASSERT (Format != NULL);

  if ((ErrorLevel & GetDebugPrintErrorLevel ()) == 0) {
    return;
  }

  UnicodeBSPrintAsciiFormat (Buffer, sizeof (Buffer), Format, BaseListMarker);

  if ((mDebugST != NULL) && (mDebugST->ConOut != NULL)) {
    gDbgCurrentLevel = ErrorLevel;
    mDebugST->ConOut->OutputString (mDebugST->ConOut, Buffer);
    gDbgCurrentLevel = GBL_DBG_LEVEL_NONE;
  }
}

/* The remaining DebugLib API stubs — identical to UefiDebugLibConOut. */

VOID
EFIAPI
DebugAssert (
  IN CONST CHAR8  *FileName,
  IN UINTN         LineNumber,
  IN CONST CHAR8  *Description
  )
{
  /* On ASSERT, forward to ConOut directly — treat as error-level output. */
  if (!mPostEBS && mDebugST != NULL && mDebugST->ConOut != NULL) {
    CHAR16 Buf[MAX_DEBUG_MESSAGE_LENGTH];
    UnicodeSPrint (Buf, sizeof (Buf),
                   L"ASSERT %a(%d): %a\n", FileName, LineNumber, Description);
    gDbgCurrentLevel = DEBUG_ERROR;
    mDebugST->ConOut->OutputString (mDebugST->ConOut, Buf);
    gDbgCurrentLevel = GBL_DBG_LEVEL_NONE;
  }

  if ((PcdGet8 (PcdDebugPropertyMask) & DEBUG_PROPERTY_ASSERT_DEADLOOP_ENABLED) != 0) {
    CpuDeadLoop ();
  }
}

VOID *
EFIAPI
DebugClearMemory (
  OUT VOID  *Address,
  IN UINTN   Length
  )
{
  return SetMem (Address, Length, PcdGet8 (PcdDebugClearMemoryValue));
}

BOOLEAN
EFIAPI
DebugAssertEnabled (VOID)
{
  return (BOOLEAN)((PcdGet8 (PcdDebugPropertyMask) &
                    DEBUG_PROPERTY_DEBUG_ASSERT_ENABLED) != 0);
}

BOOLEAN
EFIAPI
DebugPrintEnabled (VOID)
{
  return (BOOLEAN)((PcdGet8 (PcdDebugPropertyMask) &
                    DEBUG_PROPERTY_DEBUG_PRINT_ENABLED) != 0);
}

BOOLEAN
EFIAPI
DebugCodeEnabled (VOID)
{
  return (BOOLEAN)((PcdGet8 (PcdDebugPropertyMask) &
                    DEBUG_PROPERTY_DEBUG_CODE_ENABLED) != 0);
}

BOOLEAN
EFIAPI
DebugClearMemoryEnabled (VOID)
{
  return (BOOLEAN)((PcdGet8 (PcdDebugPropertyMask) &
                    DEBUG_PROPERTY_CLEAR_MEMORY_ENABLED) != 0);
}

BOOLEAN
EFIAPI
DebugPrintLevelEnabled (
  IN CONST UINTN  ErrorLevel
  )
{
  return (BOOLEAN)((ErrorLevel & PcdGet32 (PcdFixedDebugPrintErrorLevel)) != 0);
}
