/* Host test for patch7 (orange-screen) against the infiniti fixture.
   Verifies the string-anchored locate-and-rewrite (warning-string uniqueness,
   ADRP resolution, CBZ→B rewrite, target preservation, idempotency, and a
   clean MISS when the warning string is absent) AND patch7's registration in
   the active OEM aggregator table `kOemOneplusPatches[]`.  The membership
   check runs first so it executes even when the infiniti fixture is
   absent (SKIP path).  */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../../GblChainloadPkg/Include/Library/PatchDesc.h"
#include "../../GblChainloadPkg/Include/Library/ScanLib.h"
#include "../../GblChainloadPkg/Library/DynamicPatchLib/Internal/Arm64Decode.h"
#include "../../GblChainloadPkg/Library/DynamicPatchLib/oem/Signatures.h"

extern PATCH_OUTCOME ApplyOrangeScreen (UINT8 *Buf, UINT32 Size);
extern CONST PATCH_DESC  kOemOneplusPatches[];
extern CONST UINTN       kOemOneplusPatchesCount;

#ifndef TEST_FIXTURES_DIR
#error "TEST_FIXTURES_DIR must be -D'd at compile time (set by Makefile)"
#endif

/* Patch7's anchor/offset is specific to the extracted infiniti PE. The
   test runs only when that exact fixture is present; otherwise SKIP. */
#define INFINITI_FIXTURE TEST_FIXTURES_DIR "/pe/infiniti-EU-16.0.5.703.efi"

/* File offset of the CBZ instruction in the infiniti binary. */
#define PATCH7_CBZ_OFF  0x78F0U

static UINT8 *
load_file (const char *path, UINT32 *size_out)
{
  FILE *f = fopen (path, "rb");
  if (!f) {
    perror (path);
    return NULL;
  }
  fseek (f, 0, SEEK_END);
  long sz = ftell (f);
  fseek (f, 0, SEEK_SET);
  UINT8 *buf = (UINT8 *)malloc ((size_t)sz);
  if (!buf || (long)fread (buf, 1, (size_t)sz, f) != sz) {
    fclose (f);
    free (buf);
    return NULL;
  }
  fclose (f);
  *size_out = (UINT32)sz;
  return buf;
}

static UINT32
read_u32_le (const UINT8 *buf, UINT32 off)
{
  return (UINT32)buf[off + 0]
       | ((UINT32)buf[off + 1] << 8)
       | ((UINT32)buf[off + 2] << 16)
       | ((UINT32)buf[off + 3] << 24);
}

int
main (void)
{
  /* --- 0. Table membership (runs regardless of fixture presence) ---------- */
  assert (kOemOneplusPatchesCount >= 1 && "kOemOneplusPatches must contain patch7");
  int found_patch7 = 0;
  for (UINTN k = 0; k < kOemOneplusPatchesCount; ++k) {
    if (kOemOneplusPatches[k].Name != NULL
        && kOemOneplusPatches[k].Apply != NULL
        && 0 == strcmp ((const char *)kOemOneplusPatches[k].Name,
                        "patch7-orange-screen")) {
      found_patch7 = 1;
      break;
    }
  }
  assert (found_patch7 && "patch7-orange-screen not found in kOemOneplusPatches[]");
  printf ("ok patch7 table membership\n");

  UINT32 size = 0;
  UINT8 *buf  = load_file (INFINITI_FIXTURE, &size);
  if (!buf) {
    printf ("SKIP: test_patch7 — infiniti fixture %s not present\n",
            INFINITI_FIXTURE);
    return 0;
  }

  /* Pristine copy for the tier-isolation checks (section 7), taken before the
     in-place patch in section 3. */
  UINT8 *buf_orig = (UINT8 *)malloc (size);
  assert (buf_orig);
  memcpy (buf_orig, buf, size);

  /* --- 1. String anchor resolves uniquely to the guard CBZ ----------------- */
  /* The warning string is present exactly once, its ADRP+ADD load is unique,
     and the guard CBZ sits within the backward-scan window before that ADRP. */
  UINT32      str_off = 0, adrp_off = 0;
  SCAN_RESULT r = ScanFor (buf, size, (const UINT8 *)kPatch7WarnStr, NULL,
                           sizeof (kPatch7WarnStr) - 1, &str_off);
  assert (r == SCAN_FOUND && "patch7 warning string not unique");
  r = Arm64FindAdrpAddTargeting (buf, size, str_off, /*RestrictToExec=*/TRUE,
                                 &adrp_off);
  assert (r == SCAN_FOUND && "patch7 warning-string ADRP+ADD not unique");
  assert (adrp_off > PATCH7_CBZ_OFF
          && adrp_off - PATCH7_CBZ_OFF <= kPatch7BackScanWindow
          && "guard CBZ not within backward-scan window of the ADRP");
  printf ("ok patch7 string anchor (str=0x%x adrp=0x%x cbz=0x%x)\n",
          str_off, adrp_off, PATCH7_CBZ_OFF);

  /* --- 2. Pre-patch: rewrite site contains original CBZ -------------------- */
  UINT32 cbz = read_u32_le (buf, PATCH7_CBZ_OFF);
  assert ((cbz & 0xFF000000U) == 0x34000000U && "pre-patch: not a CBZ Wn");
  assert ((cbz & 0x1FU) == 10U               && "pre-patch: Rt != W10");
  printf ("ok patch7 pre-patch CBZ word 0x%08x (W%u)\n", cbz, cbz & 0x1f);

  /* --- 3. Apply patch7 ----------------------------------------------------- */
  PATCH_OUTCOME o = ApplyOrangeScreen (buf, size);
  assert (o == PATCH_OK && "patch7 first application failed");
  printf ("ok patch7 PATCH_OK\n");

  /* --- 4. Post-patch: rewrite site is an unconditional B ------------------- */
  UINT32 new_word = read_u32_le (buf, PATCH7_CBZ_OFF);
  assert ((new_word & 0xFC000000U) == 0x14000000U && "post-patch: not a B insn");
  assert (new_word == kPatch7BUnconditionalInsn    && "post-patch: B word mismatch");
  printf ("ok patch7 rewrite vs kPatch7BUnconditionalInsn (0x%08x)\n", new_word);

  /* --- 5. B-target matches original CBZ-target ----------------------------- */
  /* CBZ imm19 is bits [23:5], signed 19-bit. */
  UINT32 cbz_imm19_raw = (cbz >> 5) & 0x7FFFFU;
  INT32  cbz_imm19     = (INT32)cbz_imm19_raw;
  if (cbz_imm19 & (1 << 18)) cbz_imm19 |= ~((1 << 19) - 1);
  UINT32 cbz_target = (UINT32)((INT32)PATCH7_CBZ_OFF + cbz_imm19 * 4);

  UINT32 b_imm26_raw = new_word & 0x3FFFFFFU;
  INT32  b_imm26     = (INT32)b_imm26_raw;
  if (b_imm26 & (1 << 25)) b_imm26 |= ~((1 << 26) - 1);
  UINT32 b_target = (UINT32)((INT32)PATCH7_CBZ_OFF + b_imm26 * 4);

  assert (cbz_target == b_target && "B target != original CBZ target");
  printf ("ok patch7 B target matches CBZ target (0x%x)\n", b_target);

  /* --- 6. Idempotency: anchor still matches, second apply is PATCH_OK ------ */
  PATCH_OUTCOME o2 = ApplyOrangeScreen (buf, size);
  assert (o2 == PATCH_OK && "patch7 second application not idempotent");
  /* B word must be unchanged. */
  assert (read_u32_le (buf, PATCH7_CBZ_OFF) == kPatch7BUnconditionalInsn);
  printf ("ok patch7 idempotency\n");

  /* --- 7. No warning string -> clean MISS, no spurious rewrite ------------- */
  /* The anchor is the warning text; without it patch7 must not touch the
     binary (non-mandatory: a missing string means no orange screen to
     silence, e.g. a non-oplus ABL). */
  {
    UINT8 *b = (UINT8 *)malloc (size);
    assert (b);
    memcpy (b, buf_orig, size);
    for (UINT32 i = 0; i + sizeof (kPatch7WarnStr) - 1 < size; ++i) {
      if (0 == memcmp (b + i, kPatch7WarnStr, sizeof (kPatch7WarnStr) - 1)) {
        b[i] = 0x00;   /* break the warning string */
        break;
      }
    }
    assert (ApplyOrangeScreen (b, size) == PATCH_MISS
            && "patch7 must MISS when the warning string is absent");
    assert (read_u32_le (b, PATCH7_CBZ_OFF) == cbz
            && "patch7 must not rewrite the CBZ on a MISS");
    free (b);
    printf ("ok patch7 clean MISS without warning string\n");
  }

  free (buf);
  free (buf_orig);
  printf ("ALL PASS\n");
  return 0;
}
