/* Host test for patch9 (avb-locked-recoverable-continue) against fixtures.

   Infiniti (LinuxLoader_infiniti.efi) is the required fixture — must PATCH_OK.
   Canoe (canoe-A.07/abl_a.bin) is optional: skip if absent, required if present.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../../GblChainloadPkg/Include/Library/PatchDesc.h"
#include "../../GblChainloadPkg/Include/Library/ScanLib.h"

extern CONST PATCH_DESC kMode1Patches[];
extern CONST UINTN      kMode1PatchesCount;

#define INFINITI_FIXTURE \
  "/home/vivy/gbl-chainload/images/infiniti/LinuxLoader_infiniti.efi"
#define CANOE_FIXTURE \
  "/home/vivy/gbl-chainload/images/fixtures/canoe-A.07/abl_a.bin"

/* Expected post-patch instruction words at fixed infiniti offsets. */
#define SITE_A_OFF   0x25388U
#define SITE_B_OFF   0x25A64U
#define SITE_C_OFF   0x25C44U

#define SITE_A_EXPECTED  0x52800038U   /* MOV W24, #1              */
#define SITE_B_EXPECTED  0x34001958U   /* CBZ W24, 0x25D8C         */
#define SITE_C_EXPECTED  0x34000DB8U   /* CBZ W24, 0x25DF8         */

/* Expected CBZ branch targets (for decode-verify). */
#define SITE_B_TARGET  0x25D8CU
#define SITE_C_TARGET  0x25DF8U

static UINT8 *
load_file (const char *path, UINT32 *size_out)
{
  FILE *f = fopen (path, "rb");
  if (!f) return NULL;
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
  return (UINT32)buf[off]
       | ((UINT32)buf[off + 1] << 8)
       | ((UINT32)buf[off + 2] << 16)
       | ((UINT32)buf[off + 3] << 24);
}

static PATCH_APPLY
find_patch9 (void)
{
  for (UINTN i = 0; i < kMode1PatchesCount; ++i) {
    if (strcmp (kMode1Patches[i].Name,
                "patch9-avb-locked-recoverable-continue") == 0) {
      return kMode1Patches[i].Apply;
    }
  }
  return NULL;
}

static int
test_patch9_against (const char *path, int required)
{
  UINT32 size = 0;
  UINT8 *buf  = load_file (path, &size);
  if (!buf) {
    if (required) {
      fprintf (stderr, "REQUIRED fixture missing: %s\n", path);
      return 1;
    }
    printf ("skip patch9 against %s (file missing)\n", path);
    return 0;
  }

  PATCH_APPLY apply = find_patch9 ();
  assert (apply != NULL && "patch9 not found in kMode1Patches");

  PATCH_OUTCOME o = apply (buf, size);
  printf ("patch9 vs %s -> outcome=%d\n", path, (int)o);
  if (required) {
    assert (o == PATCH_OK && "patch9 did not return PATCH_OK");
  }

  free (buf);
  return (required && o != PATCH_OK) ? 1 : 0;
}

/* Detailed byte-level assertions against the infiniti fixture. */
static int
test_patch9_infiniti_bytes (void)
{
  UINT32 size = 0;
  UINT8 *buf  = load_file (INFINITI_FIXTURE, &size);
  if (!buf) {
    fprintf (stderr, "infiniti fixture missing — cannot run byte assertions\n");
    return 1;
  }

  PATCH_APPLY apply = find_patch9 ();
  assert (apply != NULL);

  PATCH_OUTCOME o = apply (buf, size);
  assert (o == PATCH_OK && "patch9 byte-assertion pass: apply failed");

  /* --- Site A: MOV W24, #1 = 0x52800038 little-endian --- */
  assert (buf[SITE_A_OFF + 0] == 0x38 && "Site A byte 0");
  assert (buf[SITE_A_OFF + 1] == 0x00 && "Site A byte 1");
  assert (buf[SITE_A_OFF + 2] == 0x80 && "Site A byte 2");
  assert (buf[SITE_A_OFF + 3] == 0x52 && "Site A byte 3");
  printf ("ok patch9 Site A bytes 0x%08X (expect 0x%08X)\n",
          read_u32_le (buf, SITE_A_OFF), SITE_A_EXPECTED);

  /* --- Site B: CBZ W24, 0x25D8C --- */
  {
    UINT32 word   = read_u32_le (buf, SITE_B_OFF);
    INT32  imm19  = (INT32)((word >> 5) & 0x7FFFFU);
    if (imm19 & (1 << 18)) imm19 |= ~((1 << 19) - 1);
    UINT32 target = (UINT32)((INT32)SITE_B_OFF + imm19 * 4);
    UINT32 rt     = word & 0x1FU;
    assert (word == SITE_B_EXPECTED && "Site B instruction word mismatch");
    assert (rt   == 24U             && "Site B Rt != W24");
    assert (target == SITE_B_TARGET && "Site B CBZ target mismatch");
    printf ("ok patch9 Site B word 0x%08X Rt=W%u target=0x%X\n",
            word, rt, target);
  }

  /* --- Site C: CBZ W24, 0x25DF8 --- */
  {
    UINT32 word   = read_u32_le (buf, SITE_C_OFF);
    INT32  imm19  = (INT32)((word >> 5) & 0x7FFFFU);
    if (imm19 & (1 << 18)) imm19 |= ~((1 << 19) - 1);
    UINT32 target = (UINT32)((INT32)SITE_C_OFF + imm19 * 4);
    UINT32 rt     = word & 0x1FU;
    assert (word == SITE_C_EXPECTED && "Site C instruction word mismatch");
    assert (rt   == 24U             && "Site C Rt != W24");
    assert (target == SITE_C_TARGET && "Site C CBZ target mismatch");
    printf ("ok patch9 Site C word 0x%08X Rt=W%u target=0x%X\n",
            word, rt, target);
  }

  /* --- Idempotency: apply again, outcome must still be PATCH_OK --- */
  /* After first patch the original instructions are gone, so sanity checks
     in the patch will fail — idempotency here means we don't corrupt anything
     further.  The patch returns PATCH_MISS on re-apply (sane behaviour).
     Verify the bytes are unchanged by re-reading them. */
  UINT32 a_before = read_u32_le (buf, SITE_A_OFF);
  UINT32 b_before = read_u32_le (buf, SITE_B_OFF);
  UINT32 c_before = read_u32_le (buf, SITE_C_OFF);
  /* second apply — outcome not asserted, but bytes must be unchanged */
  apply (buf, size);
  assert (read_u32_le (buf, SITE_A_OFF) == a_before && "Site A corrupted on re-apply");
  assert (read_u32_le (buf, SITE_B_OFF) == b_before && "Site B corrupted on re-apply");
  assert (read_u32_le (buf, SITE_C_OFF) == c_before && "Site C corrupted on re-apply");
  printf ("ok patch9 idempotency (bytes stable on re-apply)\n");

  free (buf);
  return 0;
}

int
main (void)
{
  int fail = 0;

  assert (find_patch9 () != NULL && "patch9 not registered in kMode1Patches");

  /* 1. Infiniti fixture — required. */
  fail += test_patch9_against (INFINITI_FIXTURE, /*required=*/1);

  /* 2. Canoe fixture — required if present, skip otherwise. */
  {
    FILE *f = fopen (CANOE_FIXTURE, "rb");
    int canoe_required = (f != NULL);
    if (f) fclose (f);
    fail += test_patch9_against (CANOE_FIXTURE, canoe_required);
  }

  /* 3. Detailed byte assertions on infiniti. */
  fail += test_patch9_infiniti_bytes ();

  if (fail) return 1;
  printf ("ALL PASS\n");
  return 0;
}
