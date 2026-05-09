/* Host test for patch7 (orange-screen) against the infiniti fixture. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../../GblChainloadPkg/Include/Library/PatchDesc.h"
#include "../../GblChainloadPkg/Include/Library/ScanLib.h"
#include "../../GblChainloadPkg/Library/DynamicPatchLib/oem/Signatures.h"

/* The patch table exported by oneplus_canoe.c. */
extern CONST PATCH_DESC kOemOneplusPatches[];
extern CONST UINTN      kOemOneplusPatchesCount;

#define INFINITI_FIXTURE \
  "/home/vivy/gbl-chainload/images/infiniti/LinuxLoader_infiniti.efi"

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
  UINT32 size = 0;
  UINT8 *buf  = load_file (INFINITI_FIXTURE, &size);
  if (!buf) {
    fprintf (stderr, "infiniti fixture missing — cannot exercise patch7\n");
    return 1;
  }

  /* --- 1. Anchor uniqueness ------------------------------------------------ */
  UINT32      anchor_off = 0;
  SCAN_RESULT r = ScanFor (buf, size,
                           kPatch7AnchorPattern, NULL,
                           kPatch7AnchorPatternLen, &anchor_off);
  assert (r == SCAN_FOUND && "patch7 anchor not unique");
  assert (anchor_off == 0x78ECU && "anchor found at unexpected offset");
  printf ("ok patch7 anchor uniqueness (off=0x%x)\n", anchor_off);

  /* --- 2. Pre-patch: rewrite site contains original CBZ -------------------- */
  UINT32 cbz = read_u32_le (buf, PATCH7_CBZ_OFF);
  assert ((cbz & 0xFF000000U) == 0x34000000U && "pre-patch: not a CBZ Wn");
  assert ((cbz & 0x1FU) == 10U               && "pre-patch: Rt != W10");
  printf ("ok patch7 pre-patch CBZ word 0x%08x (W%u)\n", cbz, cbz & 0x1f);

  /* --- 3. Apply patch7 ----------------------------------------------------- */
  PATCH_APPLY apply = NULL;
  for (UINTN i = 0; i < kOemOneplusPatchesCount; ++i) {
    if (strcmp (kOemOneplusPatches[i].Name, "patch7-orange-screen") == 0) {
      apply = kOemOneplusPatches[i].Apply;
      break;
    }
  }
  assert (apply != NULL && "patch7-orange-screen not found in kOemOneplusPatches");

  PATCH_OUTCOME o = apply (buf, size);
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
  PATCH_OUTCOME o2 = apply (buf, size);
  assert (o2 == PATCH_OK && "patch7 second application not idempotent");
  /* B word must be unchanged. */
  assert (read_u32_le (buf, PATCH7_CBZ_OFF) == kPatch7BUnconditionalInsn);
  printf ("ok patch7 idempotency\n");

  free (buf);
  printf ("ALL PASS\n");
  return 0;
}
