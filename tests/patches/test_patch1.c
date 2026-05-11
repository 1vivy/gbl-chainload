/* Host test for patch1 (efisp recursion) against discovered fixtures.

   Discovers ABL fixtures in TEST_FIXTURES_DIR (compile-time macro, default
   <repo>/tests/images, set by tests/patches/Makefile). Globs *.efi, *.bin,
   *.img and runs patch1 against each. The patch is a UTF-16 byte-scan
   ("efisp" -> "nulls"), so it works generically against either raw FV
   wrappers or extracted PEs.

   CI behaviour: zero fixtures present is acceptable — emit a SKIP line
   and exit 0. Fixture blobs are gitignored (device firmware), so CI
   normally runs with zero coverage; locally the user drops images into
   tests/images/ to exercise the patch logic.
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

extern CONST PATCH_DESC kUniversalPatches[];
extern CONST UINTN      kUniversalPatchesCount;

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

/* Returns 1 if the fixture was tested, 0 otherwise. Asserts on logic errors. */
static int
test_patch1_against (PATCH_APPLY apply, const char *path)
{
  UINT32 size = 0;
  UINT8 *buf  = load_file (path, &size);
  if (!buf) {
    return 0;
  }

  PATCH_OUTCOME o = apply (buf, size);
  printf ("patch1 vs %s -> outcome=%d\n", path, (int)o);
  assert ((o == PATCH_OK || o == PATCH_MISS) && "unexpected outcome");

  if (o == PATCH_OK) {
    UINT8 nulls_pat[] = { 'n', 0, 'u', 0, 'l', 0, 'l', 0, 's', 0 };
    int found_nulls = 0;
    for (UINT32 i = 0; i + 10 <= size; ++i) {
      if (memcmp (buf + i, nulls_pat, 10) == 0) { found_nulls = 1; break; }
    }
    assert (found_nulls && "rewrite did not produce UTF-16LE 'nulls'");

    UINT8 efisp_pat[] = { 'e', 0, 'f', 0, 'i', 0, 's', 0, 'p', 0 };
    int still_present = 0;
    for (UINT32 i = 0; i + 10 <= size; ++i) {
      if (memcmp (buf + i, efisp_pat, 10) == 0) { still_present = 1; break; }
    }
    assert (!still_present && "efisp still present after patch");

    PATCH_OUTCOME o2 = apply (buf, size);
    assert (o2 == PATCH_MISS && "second application should miss");
    printf ("ok patch1 idempotency vs %s\n", path);
  }

  free (buf);
  return 1;
}

static int
glob_extension (PATCH_APPLY apply, const char *dir, const char *ext)
{
  char pat[1024];
  snprintf (pat, sizeof (pat), "%s/*%s", dir, ext);
  glob_t g;
  int ran = 0;
  if (glob (pat, 0, NULL, &g) == 0) {
    for (size_t i = 0; i < g.gl_pathc; ++i) {
      ran += test_patch1_against (apply, g.gl_pathv[i]);
    }
    globfree (&g);
  }
  return ran;
}

int
main (void)
{
  PATCH_APPLY apply = NULL;
  for (UINTN i = 0; i < kUniversalPatchesCount; ++i) {
    if (strcmp (kUniversalPatches[i].Name, "patch1-efisp-recursion") == 0) {
      apply = kUniversalPatches[i].Apply;
      break;
    }
  }
  assert (apply != NULL && "patch1-efisp-recursion not found in kUniversalPatches");

  const char *exts[] = { ".efi", ".bin", ".img" };
  int ran = 0;
  for (size_t i = 0; i < sizeof (exts) / sizeof (exts[0]); ++i) {
    ran += glob_extension (apply, TEST_FIXTURES_DIR, exts[i]);
  }

  if (ran == 0) {
    printf ("SKIP: test_patch1 — no fixtures found in %s\n", TEST_FIXTURES_DIR);
    return 0;
  }
  printf ("ALL PASS (%d fixture%s exercised)\n", ran, ran == 1 ? "" : "s");
  return 0;
}
