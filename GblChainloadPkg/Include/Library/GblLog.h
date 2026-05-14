/** @file GblLog.h — minimal logging API for gbl-chainload.

  Two compile-time-gated macros, both with ASCII format strings:

    GBL_INFO(fmt, ...) — debug-tier emit.
      GBL_DEBUG=0: DEBUG((DEBUG_INFO, ...))  — silent log via efi_debug
      GBL_DEBUG=1: AsciiPrint(...)           — visible + log

    VERBOSE(fmt, ...) — verbose-tier emit.
      GBL_VERBOSE=0: NO-OP (compile-stripped)
      GBL_VERBOSE=1: AsciiPrint(...)         — visible + log

  Both paths reach \UefiLog<N>.txt on canoe — DEBUG via the EDK2
  DebugLib (UefiDebugLibConOut → ConOut → splitter → SerialIo → UART)
  and AsciiPrint via UEFI's Print mechanism (same downstream path).
  The "silent" property of GBL_INFO under GBL_DEBUG=0 is observed
  canoe behavior of DEBUG-level emits not dominating the framebuffer
  console. If on-device testing later contradicts that, swap DebugLib
  to a serial-only variant or re-introduce a thin screen mask.

  Single emit per call site (no UefiLog duplication). The macro picks
  ONE path per build.

  All format strings are CHAR8 ASCII ("foo=%u\n"). AsciiPrint widens
  internally for ConOut; DEBUG takes ASCII natively. Do NOT use
  L-prefixed Unicode literals with these macros.
**/
#ifndef GBL_LOG_H_
#define GBL_LOG_H_

#include <Library/UefiLib.h>    /* AsciiPrint */
#include <Library/DebugLib.h>   /* DEBUG, DEBUG_INFO */

#ifndef GBL_DEBUG
# define GBL_DEBUG 0
#endif

#ifndef GBL_VERBOSE
# define GBL_VERBOSE 0
#endif

/*
 * GBL_INFO — swap mechanism between DEBUG (silent log) and AsciiPrint
 * (visible + log). Same UefiLog destination either way.
 */
#if (GBL_DEBUG == 1)
# define GBL_INFO(fmt, ...)  AsciiPrint (fmt, ##__VA_ARGS__)
#else
# define GBL_INFO(fmt, ...)  DEBUG ((DEBUG_INFO, fmt, ##__VA_ARGS__))
#endif

/*
 * VERBOSE — hard compile-strip in non-verbose builds. Zero code at
 * call sites; format strings absent from .rodata.
 */
#if (GBL_VERBOSE == 1)
# define VERBOSE(fmt, ...)   AsciiPrint (fmt, ##__VA_ARGS__)
#else
# define VERBOSE(fmt, ...)   do { (void)0; } while (0)
#endif

#endif /* GBL_LOG_H_ */
