/** @file Signatures.h — OEM patch anchor constants for OnePlus/canoe family.

  All byte values derived from LinuxLoader_infiniti.efi (0xBE000-byte PE,
  infiniti / gbl-root-canoe build).

  Uniqueness verified: each pattern matches exactly once in that binary.
**/
#ifndef DPL_OEM_ONEPLUS_CANOE_SIGNATURES_H_
#define DPL_OEM_ONEPLUS_CANOE_SIGNATURES_H_

#include "../Internal/ScanLib.h"   /* UINT8/UINT32/UINTN incl. host shim. */

/* ---------------------------------------------------------------------------
 * Patch 7 — orange-screen / unlock-warning / 5-second boot-delay gate.
 *
 * LinuxLoader_infiniti.efi file offset 0x78EC:
 *   36 31 88 1A   AND/CSEL pair — the 4 bytes immediately preceding the CBZ.
 *
 * The CBZ that guards the orange-state block sits at AnchorOff + 4.
 * Rewriting it to an unconditional B always skips the block.
 *
 * Verified unique: exactly 1 hit in the 0xBE000-byte infiniti PE,
 * anchor match at file offset 0x78EC.
 * ---------------------------------------------------------------------------*/

STATIC CONST UINT8 kPatch7AnchorPattern[] = {
  0x36, 0x31, 0x88, 0x1A   /* CSEL / AND pair at 0x78EC */
};

#define kPatch7AnchorPatternLen  (sizeof (kPatch7AnchorPattern))

/* The CBZ instruction is at AnchorOff + this delta. */
STATIC CONST UINT32 kPatch7RewriteDelta = 4U;

/* Original CBZ word at 0x78F0: 0x3400046A  (CBZ W10, +#0x8C => target 0x797C).
   Rewrite: unconditional B with identical displacement (imm26 == imm19 == 35).
   AArch64 B encoding: 0001_01xx_xxxx_xxxx_xxxx_xxxx_xxxx_xxxx
   kPatch7BUnconditionalInsn = 0x14000000 | 35 = 0x14000023.
   B target: 0x78F0 + 35*4 = 0x797C  (same as original CBZ target). */
STATIC CONST UINT32 kPatch7BUnconditionalInsn = 0x14000023U;

#endif /* DPL_OEM_ONEPLUS_CANOE_SIGNATURES_H_ */
