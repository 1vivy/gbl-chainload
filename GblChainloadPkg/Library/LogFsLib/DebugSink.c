/** @file DebugSink.c — install/remove a wrapper around
  gST->ConOut->OutputString that mirrors every wide-string console
  write into the post-GBL log file via LogFsWrite.

  Why ConOut->OutputString: in our build DebugLib is mapped to
  GblDebugLib (GblChainloadPkg/Library/GblDebugLib), which routes
  DebugPrint() to gST->ConOut->OutputString AND records the ErrorLevel
  in gDbgCurrentLevel before doing so.  Print() does not set
  gDbgCurrentLevel, so it keeps the sentinel value (UINTN)-1 and
  always passes through to ConOut unchanged.

  Routing:
    - DEBUG((DEBUG_ERROR,...)) → gDbgCurrentLevel=DEBUG_ERROR  → both
                                 ConOut (→ QCOM BDS → UefiLog) AND logfs.
    - DEBUG((DEBUG_INFO,...))  → gDbgCurrentLevel=DEBUG_INFO   → logfs only.
    - Print(...)               → gDbgCurrentLevel=(UINTN)-1   → both (sentinel).

  Step 3 of bring-up. Step 2 already mounted logfs and opened the file.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/LogFsLib.h>
#include <Protocol/SimpleTextOut.h>

/* Set by GblDebugLib before calling ConOut->OutputString for a DEBUG() call.
 * Sentinel (UINTN)-1 means "not inside a DEBUG() call" (e.g. Print() path).
 *
 * CROSS-LIBRARY COUPLING — DO NOT BREAK SILENTLY.
 *   This extern is only satisfied when GblChainloadPkg.dsc maps the
 *   DebugLib library class to GblDebugLib.inf (the shim that defines
 *   gDbgCurrentLevel). Any future DSC variant that remaps DebugLib to a
 *   different library (UefiDebugLibConOut, DebugLibNull, etc.) will fail
 *   to link this file with an undefined-symbol error and no compile-time
 *   warning. If you re-map DebugLib, update this file too or delete the
 *   level-gating it implements.
 */
extern UINTN gDbgCurrentLevel;

STATIC EFI_TEXT_STRING gOriginalOutputString = NULL;
STATIC BOOLEAN         gInHook              = FALSE;

/* Bitmask of DEBUG() error-levels permitted to reach ConOut. Defaults to
 * DEBUG_ERROR (production: only failures + boundary markers on screen).
 * The application widens it via LogFsSetScreenMask() when GBL_DEBUG=1
 * is built. Bare Print() bypasses this gate via the sentinel value
 * in gDbgCurrentLevel. */
STATIC UINTN           gGblScreenMask       = DEBUG_ERROR;

/* Convert a UCS-2 input to ASCII in a fixed-size scratch buffer.
 * Common Unicode punctuation (em/en-dash, curly quotes) is folded to
 * its ASCII equivalent; anything else non-ASCII becomes '?'. Returns
 * the byte length written (excluding terminator). */
STATIC UINTN
Ucs2ToAscii (
  IN  CONST CHAR16 *In,
  OUT CHAR8        *Out,
  IN  UINTN         OutCap
  )
{
  UINTN i;

  if (In == NULL || Out == NULL || OutCap == 0) {
    return 0;
  }

  for (i = 0; i + 1 < OutCap && In[i] != L'\0'; i++) {
    CHAR16 Wc = In[i];
    CHAR8  Ac;

    if (Wc < 0x80) {
      Ac = (CHAR8)Wc;
    } else {
      switch (Wc) {
      case 0x2013: /* en-dash  */
      case 0x2014: /* em-dash  */
        Ac = '-';
        break;
      case 0x2018: /* left single quote  */
      case 0x2019: /* right single quote */
        Ac = '\'';
        break;
      case 0x201C: /* left double quote  */
      case 0x201D: /* right double quote */
        Ac = '"';
        break;
      default:
        Ac = '?';
        break;
      }
    }
    Out[i] = Ac;
  }
  Out[i] = '\0';
  return i;
}

STATIC EFI_STATUS EFIAPI
HookedOutputString (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
  IN CHAR16                          *String
  )
{
  EFI_STATUS Status;
  CHAR8      Buf[512];
  UINTN      Len;
  BOOLEAN    IsDebugCall;
  BOOLEAN    IsScreenLevel;

  /* Snapshot the level before any call that might alter it. */
  IsDebugCall   = (gDbgCurrentLevel != (UINTN)-1);
  IsScreenLevel = IsDebugCall && ((gDbgCurrentLevel & gGblScreenMask) != 0);

  /* Pass through to ConOut (→ QCOM BDS → UefiLog) only when:
   *   (a) this is not a DEBUG() call at all (bare Print() path) — always
   *       shown; callers use Print() for failures + user-interrupt acks
   *       that must be visible regardless of build flags, OR
   *   (b) this is a DEBUG() call at a level permitted by gGblScreenMask
   *       (DEBUG_ERROR in production, widened to INFO under GBL_DEBUG=1).
   * Levels outside the mask go to our logfs stream only. */
  if (!IsDebugCall || IsScreenLevel) {
    Status = (gOriginalOutputString != NULL)
               ? gOriginalOutputString (This, String)
               : EFI_NOT_READY;
  } else {
    Status = EFI_SUCCESS;
  }

  /* Mirror to logfs regardless of level — our private stream captures
   * everything. Guard against re-entry (LogFsWrite may emit diagnostics
   * that would re-enter OutputString via DEBUG). */
  if (!gInHook && LogFsIsReady () && String != NULL) {
    gInHook = TRUE;
    Len = Ucs2ToAscii (String, Buf, sizeof (Buf));
    if (Len > 0) {
      LogFsWrite (Buf, Len);
    }
    gInHook = FALSE;
  }

  return Status;
}

EFI_STATUS
EFIAPI
LogFsInstallDebugSink (VOID)
{
  if (gOriginalOutputString != NULL) {
    /* Already installed. */
    return EFI_ALREADY_STARTED;
  }
  if (gST == NULL || gST->ConOut == NULL ||
      gST->ConOut->OutputString == NULL) {
    return EFI_NOT_READY;
  }

  gOriginalOutputString     = gST->ConOut->OutputString;
  gST->ConOut->OutputString = HookedOutputString;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
LogFsRemoveDebugSink (VOID)
{
  if (gOriginalOutputString == NULL) {
    return EFI_NOT_STARTED;
  }
  if (gST != NULL && gST->ConOut != NULL) {
    gST->ConOut->OutputString = gOriginalOutputString;
  }
  gOriginalOutputString = NULL;
  return EFI_SUCCESS;
}

VOID
EFIAPI
LogFsSetScreenMask (
  IN UINTN  Mask
  )
{
  gGblScreenMask = Mask;
}
