/** @file Signatures.h — anchor patterns for mode-1 patches.

    patch9 v2 — Approach A: VerifyFlags-derivation rewrite + post-libavb gate
    rewrite.  See docs/re/patch9-v2-disassembly.md for the data driving
    these constants.

    The protocol-hook fakelock (Mode1Overlay) is the authoritative control
    for boot-state color (GREEN).  patch9 v2 only touches:
      Site V — make libavb permissive by always passing
                AVB_SLOT_VERIFY_FLAGS_ALLOW_VERIFICATION_ERROR (cset → mov w24,#1).
      Site G — bypass ABL's first post-libavb gate on AllowVerificationError
                (cbz → nop); "recovery-first AVB check".
      Site C — bypass ABL's second post-libavb gate on AllowVerificationError
                (cbz → nop); "common post-AVB check" ~0x1E0 bytes after Site G.
    Color computation at VerifiedBoot.c:1728 is untouched. **/

#ifndef DPL_MODE_1_SIGNATURES_H_
#define DPL_MODE_1_SIGNATURES_H_

#include "../../../Include/Library/ScanLib.h"

/* ---- Site V (VerifyFlags-derivation cset for AllowVerificationError) ---- */

/* 15 bytes; bytes [11..14] are the cset itself (0x1A9F07F8 = cset w24, ne). */
STATIC CONST UINT8 kPatch9SiteVAnchor[] = {
  0x03, 0x00, 0x71, 0xE0,
  0x03, 0x07, 0xAD, 0xE0,
  0x03, 0x08, 0xAD, 0xF8,
  0x07, 0x9F, 0x1A
};
#define kPatch9SiteVAnchorLen     15U
#define kPatch9SiteVRewriteDelta  11U          /* anchor_off + 11 = cset site */
#define kPatch9SiteVReplacement   0x52800038U  /* mov w24, #1 */

/* ---- Site G (post-libavb gate cbz on AllowVerificationError) ---- */

/* 22 bytes; the cbz is 8 bytes after the anchor end (delta = +30 from anchor
   start).  Anchor does not include the cbz, so the rewrite is naturally
   idempotent (anchor still matches after rewrite). */
STATIC CONST UINT8 kPatch9SiteGAnchor[] = {
  0xFF, 0x97, 0xE1, 0xC3,
  0x02, 0x91, 0xE4, 0x63,
  0x02, 0x91, 0xE0, 0x03,
  0x15, 0xAA, 0xE2, 0x03,
  0x16, 0xAA, 0xE3, 0x03,
  0x17, 0x2A
};
#define kPatch9SiteGAnchorLen     22U
#define kPatch9SiteGRewriteDelta  30U          /* anchor_off + 30 = cbz site */
#define kPatch9SiteGReplacement   0xD503201FU  /* nop (register-agnostic) */

/* ---- Site C (second post-libavb gate cbz on AllowVerificationError) ----
   The "common post-AVB check" — ~0x1E0 bytes after Site G in the reference.
   Anchor: 32 bytes ending just before the cbz; bytes 20-23 are the bl
   displacement (fixture-specific) and are wildcarded in the mask. */

STATIC CONST UINT8 kPatch9SiteCAnchor[] = {
  0xE8, 0xC3, 0x43, 0xF9,  /* ldr x8, [sp, #0x780]          */
  0xF8, 0x03, 0x00, 0x2A,  /* mov w24, w0  (Result)          */
  0x88, 0x00, 0x00, 0xB4,  /* cbz x8, ...  (SlotData null?)  */
  0xE0, 0x03, 0x08, 0xAA,  /* mov x0, x8                     */
  0x21, 0x00, 0x80, 0x52,  /* mov w1, #1                     */
  0x00, 0x00, 0x00, 0x00,  /* bl  ... (wildcard — bl target varies per fixture) */
  0xE8, 0x4F, 0x40, 0xF9,  /* ldr x8, [sp, #0x98]            */
  0x88, 0x02, 0x00, 0xB4,  /* cbz x8, ...  → falls to Site C */
};
STATIC CONST UINT8 kPatch9SiteCAnchorMask[] = {
  0xFF, 0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0xFF,
  0x00, 0x00, 0x00, 0x00,  /* wildcard the bl displacement    */
  0xFF, 0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0xFF,
};
#define kPatch9SiteCAnchorLen     32U
#define kPatch9SiteCRewriteDelta  32U          /* anchor_off + 32 = cbz site */
#define kPatch9SiteCReplacement   0xD503201FU  /* nop (register-agnostic) */

#endif
