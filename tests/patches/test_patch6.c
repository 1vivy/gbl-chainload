/* Host test for patch6 (lock-state fastboot-gate) against extracted PE
   fixtures.  Walks every `*.efi` in TEST_FIXTURES_DIR — these are the
   unwrapped LinuxLoader PEs (raw `*.img` FV wrappers don't satisfy the
   PE-section gate the patch uses to restrict its ADRP+ADD search, so
   they're intentionally not exercised here).

   For each .efi fixture:
     - Apply patch6, expect PATCH_OK.
     - Verify the four refusal strings are still present (the patch
       deliberately leaves .rodata alone).
     - Re-apply, expect PATCH_MISS (idempotency-via-miss: the upstream
       conditional / B.NE was rewritten, so the patch can't find the
       next gate site anymore).

   No fixtures present → emit SKIP marker and exit 0, same convention as
   the other patch tests.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <glob.h>

#include "../../GblChainloadPkg/Include/Library/PatchDesc.h"

#ifndef TEST_FIXTURES_DIR
#error "TEST_FIXTURES_DIR must be -D'd at compile time (set by Makefile)"
#endif

extern CONST PATCH_DESC kAblPermissiveFastbootGatePatches[];
extern CONST UINTN      kAblPermissiveFastbootGatePatchesCount;

static const char *const kRefusalStrs[] = {
  "Flashing is not allowed in Lock State",
  "Erase is not allowed in Lock State",
  "Slot Change is not allowed in Lock State\n",
  "Snapshot Cancel is not allowed in Lock State",
};
static const size_t kRefusalCount =
  sizeof (kRefusalStrs) / sizeof (kRefusalStrs[0]);

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

static int
buf_contains (const UINT8 *buf, UINT32 size, const char *needle)
{
  size_t nlen = strlen (needle);
  if (nlen == 0 || size < nlen) return 0;
  for (UINT32 i = 0; i + nlen <= size; ++i) {
    if (memcmp (buf + i, needle, nlen) == 0) return 1;
  }
  return 0;
}

static int
exercise_one_fixture (PATCH_APPLY apply, const char *path)
{
  UINT32 size = 0;
  UINT8 *buf = load_file (path, &size);
  if (!buf) {
    return 0;
  }

  PATCH_OUTCOME o = apply (buf, size);
  printf ("patch6 vs %s -> outcome=%d\n", path, (int)o);
  assert (o == PATCH_OK && "patch6 must apply cleanly on a supported PE");

  /* Strings stay intact — patch6 only rewrites code-section instructions. */
  for (size_t i = 0; i < kRefusalCount; ++i) {
    assert (buf_contains (buf, size, kRefusalStrs[i])
            && "refusal string disappeared after patch6");
  }

  /* Idempotency-via-miss: re-application finds no upstream conditional
     branches / B.NE forms anymore. */
  PATCH_OUTCOME o2 = apply (buf, size);
  printf ("patch6 vs %s (re-apply) -> outcome=%d\n", path, (int)o2);
  assert (o2 == PATCH_MISS
          && "patch6 must report MISS on second application");

  printf ("ok patch6 vs %s\n", path);
  free (buf);
  return 1;
}

int
main (void)
{
  PATCH_APPLY apply = NULL;
  for (UINTN i = 0; i < kAblPermissiveFastbootGatePatchesCount; ++i) {
    if (strcmp (kAblPermissiveFastbootGatePatches[i].Name, "patch6-lock-state-fastboot-gate") == 0) {
      apply = kAblPermissiveFastbootGatePatches[i].Apply;
      break;
    }
  }
  assert (apply != NULL && "patch6 not found in kAblPermissiveFastbootGatePatches");

  char pat[1024];
  snprintf (pat, sizeof (pat), "%s/*.efi", TEST_FIXTURES_DIR);
  glob_t g;
  int ran = 0;
  if (glob (pat, 0, NULL, &g) == 0) {
    for (size_t i = 0; i < g.gl_pathc; ++i) {
      ran += exercise_one_fixture (apply, g.gl_pathv[i]);
    }
    globfree (&g);
  }

  if (ran == 0) {
    printf ("SKIP: test_patch6 — no *.efi fixtures in %s "
            "(needs FV-extracted PEs)\n", TEST_FIXTURES_DIR);
    return 0;
  }
  printf ("ALL PASS (%d PE fixture%s exercised)\n", ran, ran == 1 ? "" : "s");
  return 0;
}
