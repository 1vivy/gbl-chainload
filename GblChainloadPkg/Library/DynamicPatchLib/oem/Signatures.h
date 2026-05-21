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
 * The orange-state guard instruction is byte-identical across OTA builds:
 *   0x3400046A = CBZ W10, +#0x8C  (imm19 = 35)
 * Only its surrounding code shifts between builds, so a fixed pre-CBZ anchor
 * derived from one build misses others.  The durable anchor therefore starts
 * AT the CBZ (exact 4 bytes) and discriminates with the masked 5-second-delay
 * setup that follows it (the canoe `delay_anchor`).  Rewrite site = AnchorOff
 * (the CBZ itself), so kPatch7RewriteDelta is 0.
 *
 * Verified unique (exactly 1 hit) in both PEs:
 *   - infiniti EU-16.0.5.703 PE → CBZ at file offset 0x78F0
 *   - infiniti IN-16.0.7.201 PE → CBZ at file offset 0x76D8
 *
 * Anchor includes the rewrite site, but rewriting CBZ→B leaves the trailing
 * delay-setup bytes untouched and a re-scan still matches (the masked tail is
 * unchanged), so patching stays idempotent.
 * ---------------------------------------------------------------------------*/

/* Primary anchor: the orange-state warning text.  Invariant across OTA builds
   and referenced by exactly one ADRP+ADD; the guard CBZ is the nearest CBZ Wn
   preceding that ADRP (observed at ADRP-0x1C on every oplus build, but located
   by a bounded backward scan rather than a fixed delta).  Verified unique in
   the EU-16.0.5.703, IN-16.0.7.201, and fairlady-CN-16.0.7.200 PEs. */
STATIC CONST CHAR8 kPatch7WarnStr[] = "Your device has been unlocked and can't be trusted";

/* Backward-scan window (bytes) from the warning-string ADRP to the guard CBZ. */
#define kPatch7BackScanWindow  0x40U

/* Fallback anchor (instruction pattern) for builds where the warning string is
   absent/relocated or the ADRP resolution is ambiguous. */
STATIC CONST UINT8 kPatch7AnchorPattern[] = {
  /* +0x00 */ 0x6A, 0x04, 0x00, 0x34,  /* CBZ   W10, +#0x8C  (rewrite site) */
  /* +0x04 */ 0x00, 0x06, 0x80, 0x52,  /* MOV   W?, #0x33   (delay setup)   */
  /* +0x08 */ 0x00, 0x00, 0x00, 0x00,  /* (build-variant: masked out)       */
  /* +0x0C */ 0x00, 0x05, 0x00, 0x00   /* (build-variant: masked out)       */
};

/* 0xFF = compare, 0x00 = wildcard.  The 4-byte CBZ prefix is WILDCARDED so a
   re-scan still matches after the CBZ has been rewritten to B (idempotency);
   ApplyOrangeScreen guards the rewrite site to confirm it is a CBZ (or already
   our B).  Uniqueness comes entirely from the masked delay-setup tail
   (bytes 4..15 — the canoe delay_anchor), verified to match exactly once in
   both the EU-16.0.5.703 and IN-16.0.7.201 PEs. */
STATIC CONST UINT8 kPatch7AnchorMask[] = {
  0x00, 0x00, 0x00, 0x00,   /* CBZ — wildcarded (rewrite site; guarded in code) */
  0x00, 0xFF, 0xFF, 0xFF,
  0x00, 0xFF, 0xFF, 0x00,
  0x00, 0xFF, 0x00, 0x00
};

#define kPatch7AnchorPatternLen  (sizeof (kPatch7AnchorPattern))

/* The CBZ instruction is the anchor start. */
STATIC CONST UINT32 kPatch7RewriteDelta = 0x0U;

/* CBZ word 0x3400046A (CBZ W10, +#0x8C, imm19 = 35).
   Rewrite: unconditional B with identical displacement (imm26 == imm19 == 35).
   AArch64 B encoding: 0001_01xx_xxxx_xxxx_xxxx_xxxx_xxxx_xxxx
   kPatch7BUnconditionalInsn = 0x14000000 | 35 = 0x14000023. */
STATIC CONST UINT32 kPatch7BUnconditionalInsn = 0x14000023U;

#endif /* DPL_OEM_ONEPLUS_CANOE_SIGNATURES_H_ */
