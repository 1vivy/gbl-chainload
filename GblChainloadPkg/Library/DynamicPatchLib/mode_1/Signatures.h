/** @file Signatures.h — anchor strings for mode-1 patches.

    patch10 — libavb force-AVB-success (replaces patch9 v2).
      Anchor on a unique libavb string inside `avb_slot_verify`. From the
      string ref, find the enclosing function entry (PACIASP marker) and
      rewrite two sites:
        10a — entry stash `mov wN, w3` → `orr wN, w3, #1`
              Forces bit 0 (AVB_SLOT_VERIFY_FLAGS_ALLOW_VERIFICATION_ERROR) of
              the saved flags register high, so every downstream
              `!allow_verification_error` gate inside libavb passes through.
        10c — exit return-value setup `mov w0, wM` → `mov w0, #0`
              Forces the function to return AVB_SLOT_VERIFY_RESULT_OK
              regardless of internal ret state. ABL/QcomModulePkg sees a
              clean OK + populated SlotData → boots normally.
      Together 10a + 10c subsume patch9's Site V (caller-side AVE force) and
      Site G/C (caller-side post-call gate skip). Anchor is byte-identical
      across infiniti (OnePlus 15) and myron (Xiaomi K90 Pro Max) — both
      compile the same QcomModulePkg/Library/avb/libavb source.
      See docs/project/re-findings.md for the RE pass.

    patch6 — lock-state fastboot-gate strings.  Each gate's error path loads
      one of these strings via an ADRP+ADD pair.  patch6 finds each string
      in .rodata, locates the ADRP+ADD in .text targeting it, and rewrites
      the preceding gate.  See `docs/project/re-findings.md`. **/

#ifndef DPL_MODE_1_SIGNATURES_H_
#define DPL_MODE_1_SIGNATURES_H_

#include "../../../Include/Library/ScanLib.h"

/* ---- patch10: libavb function-entry anchor string ----------------------
   Verbatim AOSP source text from
   edk2/QcomModulePkg/Library/avb/libavb/avb_slot_verify.c:1466 (the error
   message emitted when AVB_HASHTREE_ERROR_MODE_MANAGED_RESTART_AND_EIO is
   requested but the AvbOps don't implement persistent-value reads/writes).
   Used solely as a unique function-locator anchor; the patch never executes
   the codepath that emits it. */
STATIC CONST CHAR8 kPatch10AnchorStr[] =
  "Persistent values required for "
  "AVB_HASHTREE_ERROR_MODE_MANAGED_RESTART_AND_EIO";

/* AArch64 instruction words used by patch10. */
#define kArm64PaciaspWord    0xD503233FU   /* PACIASP — function-entry marker */
#define kArm64RetWord        0xD65F03C0U   /* RET */
#define kArm64MovW0Zero      0x52800000U   /* mov w0, #0 */

/* "mov wN, w3" template: 0x2A0303E0 | Rd (Rd encoded in low 5 bits).
   The COMPILER picks N (callee-saved reg) for the flags-arg stash; we mask
   to detect the pattern and extract Rd at runtime. */
#define kArm64MovFromW3Mask  0xFFFFFFE0U
#define kArm64MovFromW3Pat   0x2A0303E0U   /* mov wN, w3 (any N in low 5 bits) */

/* "orr wN, w3, #1" template: 0x32000060 | Rd. Used to rewrite the mov above
   while preserving its Rd. */
#define kArm64OrrW3OneBase   0x32000060U   /* orr wN, w3, #1 (OR Rd in low 5 bits) */

/* "mov w0, wM" template: 0x2A0003E0 | (Rm << 16).
   Match on Rd=0, Rn=WZR fields; Rm wildcarded.
   Used at function exit to locate the return-value setup. */
#define kArm64MovToW0Mask    0xFFE0FFFFU
#define kArm64MovToW0Pat     0x2A0003E0U

/* ---- patch6: lock-state fastboot-gate strings ----------------------------
   Each gate's error path loads one of these strings via an ADRP+ADD pair.
   patch6 finds each string in .rodata, locates the ADRP+ADD in .text
   targeting it, and rewrites the preceding gate.  See
   `docs/project/re-findings.md` for the byte-level treatment. */

STATIC CONST CHAR8 kPatch6FlashingStr[]       = "Flashing is not allowed in Lock State";
STATIC CONST CHAR8 kPatch6EraseStr[]          = "Erase is not allowed in Lock State";
STATIC CONST CHAR8 kPatch6SlotChangeStr[]     = "Slot Change is not allowed in Lock State\n";
STATIC CONST CHAR8 kPatch6SnapshotCancelStr[] = "Snapshot Cancel is not allowed in Lock State";

#endif
