/** @file Signatures.h — anchor patterns for mode-1 patches.

    patch9 v2 — Approach A: VerifyFlags-derivation rewrite + post-libavb gate
    rewrite.  See docs/re/patch9-v2-disassembly.md for the data driving
    these constants.

    The protocol-hook fakelock (Mode1Overlay) is the authoritative control
    for boot-state color (GREEN).  patch9 v2 only touches:
      Site V — make libavb permissive by always passing
                AVB_SLOT_VERIFY_FLAGS_ALLOW_VERIFICATION_ERROR (cset → mov w24,#1).
      Site G — bypass ABL's post-libavb gate so non-OK Result with populated
                SlotData proceeds (cbz → nop).
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

#endif
