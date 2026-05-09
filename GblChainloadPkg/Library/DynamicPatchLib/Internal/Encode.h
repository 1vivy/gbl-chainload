/** @file Encode.h — AArch64 instruction encoding helpers for patches. **/
#ifndef DPL_ENCODE_H_
#define DPL_ENCODE_H_

#include "ScanLib.h"   /* For UINT8/UINT32/BOOLEAN type aliases incl. host shim. */

VOID
WriteInstrU32 (
  IN OUT UINT8  *Buf,
  IN     UINT32  FileOff,
  IN     UINT32  Insn
  );

UINT32
ReadInstrU32 (
  IN CONST UINT8 *Buf,
  IN UINT32       FileOff
  );

/** Encode a CBZ Wn, target instruction word.
    @param InsnOff   Offset of the cbz instruction in the buffer.
    @param TargetOff Offset of the branch target.
    @param Reg       Register number (0-31).
    @param Insn      Out: encoded 32-bit instruction word.
    @return TRUE on success, FALSE if displacement out of 19-bit signed range
            or not 4-byte aligned, or Reg > 31. **/
BOOLEAN
EncodeCbz (
  IN  UINT32  InsnOff,
  IN  UINT32  TargetOff,
  IN  UINT32  Reg,
  OUT UINT32 *Insn
  );

/** Convenience: scan-target-relative CBZ rewrite.
    Combines EncodeCbz + WriteInstrU32 in one call.
    @return TRUE on successful rewrite, FALSE if displacement out of range. **/
BOOLEAN
RewriteCbz (
  IN OUT UINT8  *Buf,
  IN     UINT32  InsnOff,
  IN     UINT32  Reg,
  IN     UINT32  TargetOff
  );

#endif /* DPL_ENCODE_H_ */
