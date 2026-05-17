/** @file Signatures.h — OEM patch anchor constants for OnePlus/canoe family.

  All byte values derived from LinuxLoader_infiniti.efi (0xBE000-byte PE,
  infiniti / gbl-root-canoe build).

  Uniqueness verified: each pattern matches exactly once in that binary.
**/
#ifndef DPL_OEM_ONEPLUS_CANOE_SIGNATURES_H_
#define DPL_OEM_ONEPLUS_CANOE_SIGNATURES_H_

#include "../Internal/ScanLib.h"   /* UINT8/UINT32/UINTN incl. host shim. */

/* Shared patch bytes (kEfispUtf16Pattern, etc.) — canonical source. */
#include "../../../../tools/shared/patch_signatures.h"

/* ---------------------------------------------------------------------------
 * Patch 7 — orange-screen / unlock-warning / 5-second boot-delay gate.
 *
 * LinuxLoader_infiniti.efi file offset 0x78D8: 24-byte anchor covering 6
 * instructions of pre-CBZ context (0x78D8–0x78EF).  The CBZ that guards
 * the orange-state block sits at AnchorOff + 0x18 (= offset 0x78F0).
 * Rewriting it to an unconditional B always skips the block.
 *
 * Anchor does NOT include the rewrite site, preserving idempotency.
 *
 * Verified unique: exactly 1 hit in the 0xBE000-byte infiniti PE,
 * anchor match at file offset 0x78D8.
 * ---------------------------------------------------------------------------*/

STATIC CONST UINT8 kPatch7AnchorPattern[] = {
  /* 0x78D8 */ 0xAA, 0x16, 0x4F, 0x39,  /* LDRB  W10, [x21,#0x5]        */
  /* 0x78DC */ 0xC9, 0x0A, 0xC9, 0x1A,  /* ADC   W9, W22, W9             */
  /* 0x78E0 */ 0x1F, 0x09, 0x00, 0x71,  /* SUBS  WZR, W8, #2             */
  /* 0x78E4 */ 0xC8, 0x7E, 0x01, 0x53,  /* LSL   W8, W22, #31            */
  /* 0x78E8 */ 0xFF, 0x03, 0x08, 0xB9,  /* STR   WZR, [SP,#8]            */
  /* 0x78EC */ 0x36, 0x31, 0x88, 0x1A   /* CSEL  W22, W9, W8, CC         */
  /* 0x78F0 — rewrite site: CBZ W10, #0x8C (→ 0x797C) — NOT in anchor   */
};

#define kPatch7AnchorPatternLen  (sizeof (kPatch7AnchorPattern))

/* The CBZ instruction is at AnchorOff + this delta. */
STATIC CONST UINT32 kPatch7RewriteDelta = 0x18U;

/* Original CBZ word at 0x78F0: 0x3400046A  (CBZ W10, +#0x8C => target 0x797C).
   Rewrite: unconditional B with identical displacement (imm26 == imm19 == 35).
   AArch64 B encoding: 0001_01xx_xxxx_xxxx_xxxx_xxxx_xxxx_xxxx
   kPatch7BUnconditionalInsn = 0x14000000 | 35 = 0x14000023.
   B target: 0x78F0 + 35*4 = 0x797C  (same as original CBZ target). */
STATIC CONST UINT32 kPatch7BUnconditionalInsn = 0x14000023U;

#endif /* DPL_OEM_ONEPLUS_CANOE_SIGNATURES_H_ */
