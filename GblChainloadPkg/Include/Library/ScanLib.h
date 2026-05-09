/** @file ScanLib.h — pattern-scan helpers for DynamicPatchLib v2.
    Stateless: caller owns the buffer; helpers only read.  **/

#ifndef SCANLIB_H_
#define SCANLIB_H_

#ifndef __HOST_BUILD__
#include <Uefi.h>
#else
/* Host-build shim: define EDK-II annotation macros so the header compiles
   without the EDK-II toolchain.  Types (UINT8 etc.) come from the test file
   or from stdint.h when ScanLib.c is compiled directly by make. */
#include <stdint.h>
#include <stddef.h>
#ifndef UINT8
typedef uint8_t  UINT8;
#endif
#ifndef UINT32
typedef uint32_t UINT32;
#endif
#ifndef INT32
typedef int32_t  INT32;
#endif
#ifndef UINTN
typedef size_t   UINTN;
#endif
#ifndef BOOLEAN
typedef int      BOOLEAN;
#endif
#ifndef VOID
#define VOID void
#endif
#ifndef CONST
#define CONST const
#endif
#ifndef IN
#define IN
#endif
#ifndef OUT
#define OUT
#endif
#ifndef OPTIONAL
#define OPTIONAL
#endif
#ifndef STATIC
#define STATIC static
#endif
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#endif /* __HOST_BUILD__ */

typedef enum {
  SCAN_FOUND       = 0,   // exactly one match in the scanned domain
  SCAN_NOT_FOUND   = 1,
  SCAN_AMBIGUOUS   = 2,   // >1 match — pattern not unique
  SCAN_BAD_INPUT   = 3,   // NULL pointer or zero-size buffer
} SCAN_RESULT;

/** Scan the entire buffer for exactly one match of Pattern.
    @param Buf,Size       Buffer to scan.
    @param Pattern        Bytes to match.
    @param Mask           Optional per-byte mask: 0xFF = compare, 0x00 = wildcard.
                          NULL = exact-match (all 0xFF mask).
    @param PatternLen     Length of Pattern (and Mask if non-NULL).
    @param MatchOff       Out: file-offset of unique match (only on SCAN_FOUND).
    @return SCAN_FOUND / NOT_FOUND / AMBIGUOUS / BAD_INPUT.
    Always scans the whole buffer (no first-match exit) so ambiguity is detected. **/
SCAN_RESULT
ScanFor (
  IN  CONST UINT8 *Buf,
  IN  UINT32       Size,
  IN  CONST UINT8 *Pattern,
  IN  CONST UINT8 *Mask OPTIONAL,
  IN  UINTN        PatternLen,
  OUT UINT32      *MatchOff
  );

/** Same as ScanFor, but restricted to file-offsets that lie inside
    an executable PE section.  Useful for code-only patch anchors.
    When ExecOnly is FALSE, behaviour is identical to ScanFor. **/
SCAN_RESULT
ScanForBoundedSection (
  IN  CONST UINT8 *Buf,
  IN  UINT32       Size,
  IN  BOOLEAN      ExecOnly,
  IN  CONST UINT8 *Pattern,
  IN  CONST UINT8 *Mask OPTIONAL,
  IN  UINTN        PatternLen,
  OUT UINT32      *MatchOff
  );

#endif /* SCANLIB_H_ */
