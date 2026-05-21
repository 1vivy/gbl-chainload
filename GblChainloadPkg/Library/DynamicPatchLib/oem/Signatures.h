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
 * Anchored on the orange-state warning text, which is invariant across OTA
 * builds and referenced by exactly one ADRP+ADD.  ApplyOrangeScreen resolves
 * that pair, walks backward to the nearest CBZ Wn (the lock-state guard) and
 * rewrites it to an unconditional B so the warning + 5-second delay block is
 * always skipped.  The guard CBZ is 0x3400046A (CBZ W10, +#0x8C) — byte-
 * identical across builds; only its surrounding code shifts, which is why a
 * fixed byte anchor failed and the string walk is used instead.
 *
 * Verified: EU-16.0.5.703 (CBZ @0x78F0), IN-16.0.7.201 (@0x76D8),
 * fairlady-CN-16.0.7.200 (@0x76D8).  The intervening instructions between the
 * CBZ and the ADRP are X-form CBZ/CBNZ, stepped over by the W-form match.
 * ---------------------------------------------------------------------------*/

/* The orange-state warning text.  "Orange State" alone has two ADRP refs
   (ambiguous); this longer line is referenced exactly once and its ADRP sits
   just past the guard CBZ. */
STATIC CONST CHAR8 kPatch7WarnStr[] = "Your device has been unlocked and can't be trusted";

/* Backward-scan window (bytes) from the warning-string ADRP to the guard CBZ.
   The guard is observed at ADRP-0x1C on every oplus build; this window leaves
   ample margin while staying inside the warning-render prologue. */
#define kPatch7BackScanWindow  0x40U

/* CBZ word 0x3400046A (CBZ W10, +#0x8C). RewriteBUncond preserves the target,
   yielding B with imm26 == 35 == 0x14000023; kept here for the host test. */
STATIC CONST UINT32 kPatch7BUnconditionalInsn = 0x14000023U;

#endif /* DPL_OEM_ONEPLUS_CANOE_SIGNATURES_H_ */
