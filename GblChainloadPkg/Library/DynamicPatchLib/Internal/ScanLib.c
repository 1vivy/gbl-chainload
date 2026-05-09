/** @file ScanLib.c — pattern scanner.
    Always scans the whole buffer to detect ambiguity. **/
#include "ScanLib.h"

STATIC BOOLEAN
MatchAt (
  CONST UINT8 *Buf,
  CONST UINT8 *Pattern,
  CONST UINT8 *Mask,
  UINTN        PatternLen
  )
{
  UINTN i;
  for (i = 0; i < PatternLen; ++i) {
    UINT8 b = Buf[i] ^ Pattern[i];
    UINT8 m = (Mask != NULL) ? Mask[i] : 0xFF;
    if ((b & m) != 0) return FALSE;
  }
  return TRUE;
}

SCAN_RESULT
ScanFor (
  IN  CONST UINT8 *Buf,
  IN  UINT32       Size,
  IN  CONST UINT8 *Pattern,
  IN  CONST UINT8 *Mask OPTIONAL,
  IN  UINTN        PatternLen,
  OUT UINT32      *MatchOff
  )
{
  UINT32 i;
  UINT32 Found = 0;
  UINT32 FirstOff = 0;

  if (Buf == NULL || Pattern == NULL || MatchOff == NULL ||
      PatternLen == 0 || Size < PatternLen) {
    return SCAN_BAD_INPUT;
  }

  for (i = 0; i + PatternLen <= Size; ++i) {
    if (MatchAt (Buf + i, Pattern, Mask, PatternLen)) {
      if (Found == 0) FirstOff = i;
      ++Found;
    }
  }

  if (Found == 0) return SCAN_NOT_FOUND;
  if (Found > 1)  return SCAN_AMBIGUOUS;
  *MatchOff = FirstOff;
  return SCAN_FOUND;
}
