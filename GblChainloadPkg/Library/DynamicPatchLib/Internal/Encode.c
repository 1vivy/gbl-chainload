/** @file Encode.c — AArch64 instruction encoding helpers. **/
#include "Encode.h"

VOID
WriteInstrU32 (
  IN OUT UINT8  *Buf,
  IN     UINT32  FileOff,
  IN     UINT32  Insn
  )
{
  Buf[FileOff + 0] = (UINT8)(Insn >>  0);
  Buf[FileOff + 1] = (UINT8)(Insn >>  8);
  Buf[FileOff + 2] = (UINT8)(Insn >> 16);
  Buf[FileOff + 3] = (UINT8)(Insn >> 24);
}

UINT32
ReadInstrU32 (
  IN CONST UINT8 *Buf,
  IN UINT32       FileOff
  )
{
  return ((UINT32)Buf[FileOff + 0] <<  0) |
         ((UINT32)Buf[FileOff + 1] <<  8) |
         ((UINT32)Buf[FileOff + 2] << 16) |
         ((UINT32)Buf[FileOff + 3] << 24);
}

BOOLEAN
EncodeCbz (
  IN  UINT32  InsnOff,
  IN  UINT32  TargetOff,
  IN  UINT32  Reg,
  OUT UINT32 *Insn
  )
{
  INT32 Delta;

  if (Reg > 31) {
    return FALSE;
  }

  /* Signed byte displacement from instruction to target. */
  Delta = (INT32)(TargetOff - InsnOff);

  /* Must be 4-byte aligned. */
  if ((Delta & 3) != 0) {
    return FALSE;
  }

  /* Convert to instruction-word delta (divide by 4). */
  Delta >>= 2;

  /* 19-bit signed range: [-2^18, 2^18 - 1]. */
  if (Delta < -(1 << 18) || Delta >= (1 << 18)) {
    return FALSE;
  }

  /* CBZ Wn encoding: [31:25]=0b0011010, sf=0, imm19=[23:5], Rt=[4:0].
     Top byte = 0x34 for CBZ Wn (32-bit register, sf=0). */
  *Insn = 0x34000000U | (((UINT32)Delta & 0x7FFFFu) << 5) | (Reg & 0x1Fu);
  return TRUE;
}

BOOLEAN
RewriteCbz (
  IN OUT UINT8  *Buf,
  IN     UINT32  InsnOff,
  IN     UINT32  Reg,
  IN     UINT32  TargetOff
  )
{
  UINT32 Insn;

  if (!EncodeCbz (InsnOff, TargetOff, Reg, &Insn)) {
    return FALSE;
  }

  WriteInstrU32 (Buf, InsnOff, Insn);
  return TRUE;
}
