/* Host test for patch9 v2 (Approach A) against all available PE fixtures.

   Spec stop-line (docs/re/patch9-v2-disassembly.md): ≥3 of 5 PE fixtures
   must hit PATCH_OK with byte-equivalent post-patch results at Site V and
   Site G offsets. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../../GblChainloadPkg/Include/Library/PatchDesc.h"

extern CONST PATCH_DESC kMode1Patches[];
extern CONST UINTN      kMode1PatchesCount;

static UINT8 *load_file (const char *path, UINT32 *size_out) {
  FILE *f = fopen (path, "rb");
  if (!f) return NULL;
  fseek (f, 0, SEEK_END);
  long sz = ftell (f);
  fseek (f, 0, SEEK_SET);
  UINT8 *buf = (UINT8 *)malloc ((size_t)sz);
  fread (buf, 1, (size_t)sz, f);
  fclose (f);
  *size_out = (UINT32)sz;
  return buf;
}

typedef struct {
  CONST char *path;
  CONST char *label;
  int         expect_required;   /* 1 = must PATCH_OK; 0 = MISS allowed */
  /* Per-fixture expected post-byte words at Site V, Site G, and Site C offsets.
     Site V: 0x52800038 (mov w24,#1)
     Site G: 0xD503201F (nop)
     Site C: 0xD503201F (nop) */
  UINT32      site_v_off;
  UINT32      site_g_off;
  UINT32      site_c_off;
} fixture_t;

static fixture_t fixtures[] = {
  { .path = "/home/vivy/gbl-chainload/images/infiniti/LinuxLoader_infiniti.efi",
    .label = "infiniti (reference)",
    .expect_required = 1,
    .site_v_off = 0x25388U, .site_g_off = 0x25A64U, .site_c_off = 0x25C44U },
  { .path = "/home/vivy/gbl-chainload/images/pe/infiniti-EU-16.0.5.703.efi",
    .label = "infiniti-EU-16.0.5.703",
    .expect_required = 1,
    .site_v_off = 0x253DCU, .site_g_off = 0x25AB8U, .site_c_off = 0x25C98U },
  { .path = "/home/vivy/gbl-chainload/images/pe/infiniti-IN-16.0.7.201.efi",
    .label = "infiniti-IN-16.0.7.201",
    .expect_required = 1,
    .site_v_off = 0x238C4U, .site_g_off = 0x23FF4U, .site_c_off = 0x241D4U },
  { .path = "/home/vivy/gbl-chainload/images/pe/fairlady-CN-16.0.7.200.efi",
    .label = "fairlady-CN-16.0.7.200",
    .expect_required = 1,
    .site_v_off = 0x23654U, .site_g_off = 0x23D84U, .site_c_off = 0x23F64U },
  { .path = "/home/vivy/gbl-chainload/images/pe/myron.efi",
    .label = "myron (no libavb path expected)",
    .expect_required = 0,
    .site_v_off = 0, .site_g_off = 0, .site_c_off = 0 },
};

#define EXPECTED_SITE_V_WORD  0x52800038U  /* mov w24, #1 */
#define EXPECTED_SITE_G_WORD  0xD503201FU  /* nop */

static UINT32 read_le32 (CONST UINT8 *p) {
  return (UINT32)p[0] | ((UINT32)p[1] << 8)
       | ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}

int main (void) {
  PATCH_APPLY apply = NULL;
  for (UINTN i = 0; i < kMode1PatchesCount; ++i) {
    if (strcmp (kMode1Patches[i].Name, "patch9-avb-locked-recoverable-continue") == 0) {
      apply = kMode1Patches[i].Apply;
      break;
    }
  }
  assert (apply != NULL);

  int passed = 0;
  int failed = 0;
  int skipped = 0;

  for (size_t i = 0; i < sizeof (fixtures) / sizeof (fixtures[0]); ++i) {
    fixture_t *fx = &fixtures[i];
    UINT32 size = 0;
    UINT8 *buf = load_file (fx->path, &size);
    if (!buf) {
      printf ("skip %-40s — file missing\n", fx->label);
      ++skipped;
      continue;
    }

    PATCH_OUTCOME o = apply (buf, size);

    if (o == PATCH_OK) {
      if (!fx->expect_required) {
        printf ("FAIL %-40s — PATCH_OK on a fixture marked optional/no-libavb\n", fx->label);
        ++failed;
        free (buf); continue;
      }
      /* Verify byte-equivalent post-patch at Site V, Site G, and Site C. */
      UINT32 vbytes = read_le32 (buf + fx->site_v_off);
      UINT32 gbytes = read_le32 (buf + fx->site_g_off);
      UINT32 cbytes = read_le32 (buf + fx->site_c_off);
      int byte_check = 1;
      if (vbytes != EXPECTED_SITE_V_WORD) {
        printf ("FAIL %-40s — Site V @0x%x: got 0x%08x expected 0x%08x\n",
                fx->label, fx->site_v_off, vbytes, EXPECTED_SITE_V_WORD);
        byte_check = 0;
      }
      if (gbytes != EXPECTED_SITE_G_WORD) {
        printf ("FAIL %-40s — Site G @0x%x: got 0x%08x expected 0x%08x\n",
                fx->label, fx->site_g_off, gbytes, EXPECTED_SITE_G_WORD);
        byte_check = 0;
      }
      if (cbytes != EXPECTED_SITE_G_WORD) {
        printf ("FAIL %-40s — Site C @0x%x: got 0x%08x expected 0x%08x\n",
                fx->label, fx->site_c_off, cbytes, EXPECTED_SITE_G_WORD);
        byte_check = 0;
      }
      if (byte_check) {
        printf ("ok   %-40s — PATCH_OK + Site V/G/C post-bytes match\n", fx->label);
        ++passed;
      } else {
        ++failed;
      }
    } else {
      /* PATCH_MISS or PATCH_AMBIGUOUS. */
      const char *outcome_name =
        (o == PATCH_MISS)      ? "MISS" :
        (o == PATCH_AMBIGUOUS) ? "AMBIGUOUS" : "?";
      if (fx->expect_required) {
        printf ("FAIL %-40s — expected PATCH_OK, got %s\n", fx->label, outcome_name);
        ++failed;
      } else {
        printf ("ok   %-40s — %s acceptable (no libavb path)\n", fx->label, outcome_name);
        ++passed;
      }
    }
    free (buf);
  }

  printf ("---\n%d passed, %d failed, %d skipped\n", passed, failed, skipped);

  /* Spec stop-line: ≥3 PATCH_OK across 5 PE fixtures. */
  int patch_ok_required = 0;
  for (size_t i = 0; i < sizeof (fixtures) / sizeof (fixtures[0]); ++i) {
    if (fixtures[i].expect_required) ++patch_ok_required;
  }
  if (patch_ok_required - failed < 3) {
    printf ("FAIL: <3 fixtures hit PATCH_OK; spec stop-line violated\n");
    return 1;
  }

  return failed == 0 ? 0 : 1;
}
