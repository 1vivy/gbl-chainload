/** @file Signatures.h — mode-1 patch anchor constants.

  All byte values derived from LinuxLoader_infiniti.efi (0xBE000-byte PE,
  infiniti / gbl-root-canoe build).

  Uniqueness verified: each pattern matches exactly once in that binary's
  executable section.

  Patch 9 — avb-locked-recoverable-continue.

  Three rewrite sites in libavb's LoadImageAndAuthVB2 (and helpers):
    A. VerifyFlagsInit at 0x25388: cset w24,ne   -> mov w24,#1
    B. RecoveryGate   at 0x25A64: cbz w29,<T1>  -> cbz w24,<T1'>
    C. CommonGate     at 0x25C44: cbz w29,<T2>  -> cbz w24,<T2'>

  New CBZ targets are source-level labels stable per compilation:
    T1' = 0x25D8C  (RecoveryGate + 0x328)
    T2' = 0x25DF8  (CommonGate   + 0x1B4)

  Each anchor is the 32 bytes ending just before its rewrite site
  (rewrite delta = 0x20), so patching is idempotent.
**/
#ifndef DPL_MODE_1_SIGNATURES_H_
#define DPL_MODE_1_SIGNATURES_H_

#include "../Internal/ScanLib.h"   /* UINT8/UINT32/UINTN incl. host shim. */

/* ---------------------------------------------------------------------------
 * Site A — VerifyFlagsInit anchor
 *
 * LinuxLoader_infiniti.efi file offset 0x25368: 32 bytes of pre-context
 * covering 8 instructions (0x25368–0x25387).  Rewrite site at 0x25388
 * (= AnchorOff + 0x20).
 *
 * Verified unique: exactly 1 hit at 0x25368 in the infiniti executable section.
 * ---------------------------------------------------------------------------*/
STATIC CONST UINT8 kPatch9AnchorA[] = {
  /* 0x25368 */ 0x68, 0x00, 0x80, 0x52,  /* MOV   W8, #3                   */
  /* 0x2536C */ 0x7D, 0x17, 0x4F, 0x39,  /* LDRB  W29, [X27,#0x5]          */
  /* 0x25370 */ 0xFF, 0x4F, 0x00, 0xF9,  /* STR   XZR, [SP,#0x98]          */
  /* 0x25374 */ 0xFF, 0xA3, 0x00, 0xF9,  /* STR   XZR, [SP,#0x140]         */
  /* 0x25378 */ 0xE0, 0x03, 0x06, 0xAD,  /* STP   Q0, Q1, [SP,#0x60]       */
  /* 0x2537C */ 0xBF, 0x03, 0x00, 0x71,  /* SUBS  WZR, W29, #0             */
  /* 0x25380 */ 0xE0, 0x03, 0x07, 0xAD,  /* STP   Q0, Q1, [SP,#0x70]       */
  /* 0x25384 */ 0xE0, 0x03, 0x08, 0xAD   /* STP   Q0, Q1, [SP,#0x80]       */
  /* 0x25388 — rewrite site: CSET W24, NE (0x1A9F07F8) — NOT in anchor     */
};
#define kPatch9AnchorALen        (sizeof (kPatch9AnchorA))
#define kPatch9SiteARewriteDelta  0x20U   /* anchor_off + 0x20 = 0x25388 */
#define kPatch9SiteAOriginalInsn  0x1A9F07F8U  /* CSET W24, NE */
#define kPatch9SiteAReplacementInsn  0x52800038U   /* MOV W24, #1 */

/* ---------------------------------------------------------------------------
 * Site B — RecoveryGate anchor
 *
 * File offset 0x25A44: 32 bytes ending at 0x25A63.  Rewrite site at 0x25A64
 * (= AnchorOff + 0x20).
 *
 * Verified unique: exactly 1 hit at 0x25A44 in the infiniti executable section.
 * ---------------------------------------------------------------------------*/
STATIC CONST UINT8 kPatch9AnchorB[] = {
  /* 0x25A44 */ 0x58, 0xF6, 0xFF, 0x97,  /* BL   avb_slot_verify (stub)    */
  /* 0x25A48 */ 0xE1, 0xC3, 0x02, 0x91,  /* ADD  X1, SP, #0xB0             */
  /* 0x25A4C */ 0xE4, 0x63, 0x02, 0x91,  /* ADD  X4, SP, #0x98             */
  /* 0x25A50 */ 0xE0, 0x03, 0x15, 0xAA,  /* MOV  X0, X21                   */
  /* 0x25A54 */ 0xE2, 0x03, 0x16, 0xAA,  /* MOV  X2, X22                   */
  /* 0x25A58 */ 0xE3, 0x03, 0x17, 0x2A,  /* MOV  W3, W23                   */
  /* 0x25A5C */ 0x8B, 0x14, 0x00, 0x94,  /* BL   avb_result_should_continue */
  /* 0x25A60 */ 0xF8, 0x03, 0x00, 0x2A   /* MOV  W24, W0                   */
  /* 0x25A64 — rewrite site: CBZ W29, 0x25D58 (0x340017BD) — NOT in anchor */
};
#define kPatch9AnchorBLen        (sizeof (kPatch9AnchorB))
#define kPatch9SiteBRewriteDelta  0x20U   /* anchor_off + 0x20 = 0x25A64 */
#define kPatch9SiteBOriginalInsn  0x340017BDU  /* CBZ W29, ... */
#define kPatch9SiteBTargetDelta   0x328U  /* target = rewrite_off + 0x328 = 0x25D8C */

/* ---------------------------------------------------------------------------
 * Site C — CommonGate anchor
 *
 * File offset 0x25C24: 32 bytes ending at 0x25C43.  Rewrite site at 0x25C44
 * (= AnchorOff + 0x20).
 *
 * Verified unique: exactly 1 hit at 0x25C24 in the infiniti executable section.
 * ---------------------------------------------------------------------------*/
STATIC CONST UINT8 kPatch9AnchorC[] = {
  /* 0x25C24 */ 0xE8, 0xC3, 0x43, 0xF9,  /* LDR  X8, [SP,#0x780]           */
  /* 0x25C28 */ 0xF8, 0x03, 0x00, 0x2A,  /* MOV  W24, W0                   */
  /* 0x25C2C */ 0x88, 0x00, 0x00, 0xB4,  /* CBZ  X8, +0x10                 */
  /* 0x25C30 */ 0xE0, 0x03, 0x08, 0xAA,  /* MOV  X0, X8                    */
  /* 0x25C34 */ 0x21, 0x00, 0x80, 0x52,  /* MOV  W1, #1                    */
  /* 0x25C38 */ 0x93, 0x77, 0xFF, 0x97,  /* BL   avb_free_slot_data (stub) */
  /* 0x25C3C */ 0xE8, 0x4F, 0x40, 0xF9,  /* LDR  X8, [SP,#0x98]            */
  /* 0x25C40 */ 0x88, 0x02, 0x00, 0xB4   /* CBZ  X8, +0x50                 */
  /* 0x25C44 — rewrite site: CBZ W29, 0x25D04 (0x3400061D) — NOT in anchor */
};
#define kPatch9AnchorCLen        (sizeof (kPatch9AnchorC))
#define kPatch9SiteCRewriteDelta  0x20U   /* anchor_off + 0x20 = 0x25C44 */
#define kPatch9SiteCOriginalInsn  0x3400061DU  /* CBZ W29, ... */
#define kPatch9SiteCTargetDelta   0x1B4U  /* target = rewrite_off + 0x1B4 = 0x25DF8 */

#endif /* DPL_MODE_1_SIGNATURES_H_ */
