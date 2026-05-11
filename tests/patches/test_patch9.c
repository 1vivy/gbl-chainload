/* Host test for patch9 v2 (Approach A) against all available PE fixtures.

   Spec stop-line (docs/re/patch9-v2-disassembly.md): ≥3 of 5 PE fixtures
   must hit PATCH_OK with byte-equivalent post-patch results at Site V and
   Site G offsets. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../../GblChainloadPkg/Include/Library/PatchDesc.h"

#ifndef TEST_FIXTURES_DIR
#error "TEST_FIXTURES_DIR must be -D'd at compile time (set by Makefile)"
#endif

extern CONST PATCH_DESC kMode1Patches[];
extern CONST UINTN      kMode1PatchesCount;

static UINT8 *load_file (const char *path, UINT32 *size_out) {
  FILE *f = fopen (path, "rb");
  if (!f) return NULL;
  fseek (f, 0, SEEK_END);
  long sz = ftell (f);
  fseek (f, 0, SEEK_SET);
  UINT8 *buf = (UINT8 *)malloc ((size_t)sz);
  if (!buf || (long)fread (buf, 1, (size_t)sz, f) != sz) {
    fclose (f); free (buf); return NULL;
  }
  fclose (f);
  *size_out = (UINT32)sz;
  return buf;
}

typedef struct {
  /* Filename within TEST_FIXTURES_DIR. Resolved to a full path at runtime. */
  CONST char *file;
  CONST char *label;
  int         expect_required;   /* 1 = must PATCH_OK; 0 = MISS allowed */
  /* Per-fixture expected post-byte words at Site V, Site G, Site C.
     Site V: 0x52800038 (mov w24,#1)
     Site G/C: 0xD503201F (nop) */
  UINT32      site_v_off;
  UINT32      site_g_off;
  UINT32      site_c_off;
} fixture_t;

/* Offsets below are valid for extracted PE form of each fixture. Raw FV
   wrappers don't expose these PE-relative offsets — for that case the
   fixture's PE must be extracted first (see scripts/extract-pe-from-fv.sh)
   and dropped into TEST_FIXTURES_DIR with the matching filename. */
static fixture_t fixtures[] = {
  { .file = "LinuxLoader_infiniti.efi",
    .label = "infiniti (reference)",
    .expect_required = 1,
    .site_v_off = 0x25388U, .site_g_off = 0x25A64U, .site_c_off = 0x25C44U },
  { .file = "infiniti-EU-16.0.5.703.efi",
    .label = "infiniti-EU-16.0.5.703",
    .expect_required = 1,
    .site_v_off = 0x253DCU, .site_g_off = 0x25AB8U, .site_c_off = 0x25C98U },
  { .file = "infiniti-IN-16.0.7.201.efi",
    .label = "infiniti-IN-16.0.7.201",
    .expect_required = 1,
    .site_v_off = 0x238C4U, .site_g_off = 0x23FF4U, .site_c_off = 0x241D4U },
  { .file = "fairlady-CN-16.0.7.200.efi",
    .label = "fairlady-CN-16.0.7.200",
    .expect_required = 1,
    .site_v_off = 0x23654U, .site_g_off = 0x23D84U, .site_c_off = 0x23F64U },
  { .file = "myron.efi",
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

  char path[1024];
  for (size_t i = 0; i < sizeof (fixtures) / sizeof (fixtures[0]); ++i) {
    fixture_t *fx = &fixtures[i];
    snprintf (path, sizeof (path), "%s/%s", TEST_FIXTURES_DIR, fx->file);
    UINT32 size = 0;
    UINT8 *buf = load_file (path, &size);
    if (!buf) {
      printf ("skip %-40s — %s not present\n", fx->label, path);
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

  if (failed > 0) return 1;

  /* Zero fixtures actually exercised — emit a SKIP marker so CI logs make
     it obvious that the test ran without coverage (fixtures are gitignored
     device blobs; CI normally won't have them). */
  if (passed == 0) {
    printf ("SKIP: test_patch9 — no fixtures present in %s\n",
            TEST_FIXTURES_DIR);
    return 0;
  }

  return 0;
}
