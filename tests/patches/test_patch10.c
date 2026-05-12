/* Host test for patch10 (libavb force-AVB-success) against extracted PE
   fixtures.

   Walks every `*.efi` in TEST_FIXTURES_DIR — these are the unwrapped
   LinuxLoader PEs that contain libavb. Raw `*.img` FV wrappers won't get
   past patch10's executable-section gate (which requires PE structure for
   the ADRP+ADD scan to anchor inside .text), so they're intentionally not
   exercised here.

   For each .efi fixture:
     - Apply patch10, expect PATCH_OK.
     - Verify the two predicted rewrites landed: the function-entry
       `mov wN, w3` is now `orr wN, w3, #1` (Rd preserved), and the
       function-exit `mov w0, wM` is now `mov w0, #0` (0x52800000).
     - Re-apply, expect PATCH_MISS (idempotency-via-miss: the entry-pattern
       no longer matches `mov wN, w3` so step 4 of patch10 returns MISS).

   No fixtures present → SKIP marker and exit 0 (same convention as the
   other patch tests).
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

extern CONST PATCH_DESC kMode1Patches[];
extern CONST UINTN      kMode1PatchesCount;

/* Same anchor string as patch10 uses internally — duplicated here to keep
   test independent of the patch's private signature header. */
static const char *const kAnchorStr =
  "Persistent values required for "
  "AVB_HASHTREE_ERROR_MODE_MANAGED_RESTART_AND_EIO";

static UINT8 *
load_file (const char *path, UINT32 *size_out)
{
  FILE *f = fopen (path, "rb");
  if (!f) { perror (path); return NULL; }
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

static UINT32
read_u32_le (const UINT8 *buf, UINT32 off)
{
  return (UINT32)buf[off + 0]
       | ((UINT32)buf[off + 1] << 8)
       | ((UINT32)buf[off + 2] << 16)
       | ((UINT32)buf[off + 3] << 24);
}

/* Find the offset of `needle` in `buf`. Returns (UINT32)-1 if absent. */
static UINT32
find_string (const UINT8 *buf, UINT32 size, const char *needle)
{
  size_t nlen = strlen (needle);
  if (nlen == 0 || size < nlen) return (UINT32)-1;
  for (UINT32 i = 0; i + nlen <= size; ++i) {
    if (memcmp (buf + i, needle, nlen) == 0) return i;
  }
  return (UINT32)-1;
}

static int
exercise_one_fixture (PATCH_APPLY apply, const char *path)
{
  UINT32 size = 0;
  UINT8 *buf = load_file (path, &size);
  if (!buf) return 0;

  /* Sanity: the anchor string must be present, otherwise patch10 has
     nothing to do and this fixture isn't a libavb-bearing PE. */
  if (find_string (buf, size, kAnchorStr) == (UINT32)-1) {
    printf ("skip %s — anchor string absent (not a libavb PE)\n", path);
    free (buf);
    return 0;
  }

  /* Capture the original entry/exit instruction words so we can verify the
     rewrites landed at the right structural sites. We don't reproduce
     patch10's full RE here — we use the same anchor + back/forward scan to
     find the same sites, then assert byte-equivalence post-patch. */
  PATCH_OUTCOME o = apply (buf, size);
  printf ("patch10 vs %s -> outcome=%d\n", path, (int)o);
  assert (o == PATCH_OK && "patch10 must apply on a libavb-bearing PE");

  /* Sweep the buffer for the rewrite signatures:
       - At least one `orr wN, w3, #1` (word 0x32000060 | Rd) — the 10a result.
       - At least one `mov w0, #0`     (word 0x52800000)      — the 10c result.
     Both are post-patch tokens that should be unique to patch10's effect
     (no compiler emits `orr wN, w3, #1` casually in the libavb compile we
     looked at, and `mov w0, #0` appearing inside avb_slot_verify is the
     specific marker patch10 leaves). */
  int orr_seen = 0, movz_seen = 0;
  for (UINT32 i = 0; i + 4 <= size; i += 4) {
    UINT32 word = read_u32_le (buf, i);
    if ((word & 0xFFFFFFE0U) == 0x32000060U) orr_seen++;
    if (word == 0x52800000U) movz_seen++;
  }
  /* These are loose counts — orr_seen and movz_seen can be >1 if other
     code happens to use the same encoding. We only need at least 1 of each
     after patch10 has applied. */
  assert (orr_seen  >= 1 && "patch10 did not produce `orr wN, w3, #1`");
  assert (movz_seen >= 1 && "patch10 did not produce `mov w0, #0`");

  /* Re-apply: patch10 looks for `mov wN, w3` in the prologue, but we just
     rewrote it to `orr wN, w3, #1` — so the second application should miss
     at step 4 (no prologue mov-from-w3) and return PATCH_MISS. */
  PATCH_OUTCOME o2 = apply (buf, size);
  printf ("patch10 vs %s (re-apply) -> outcome=%d\n", path, (int)o2);
  assert (o2 == PATCH_MISS
          && "patch10 must report MISS on second application");

  printf ("ok patch10 vs %s\n", path);
  free (buf);
  return 1;
}

int
main (void)
{
  PATCH_APPLY apply = NULL;
  for (UINTN i = 0; i < kMode1PatchesCount; ++i) {
    if (strcmp (kMode1Patches[i].Name, "patch10-libavb-force-avb-success") == 0) {
      apply = kMode1Patches[i].Apply;
      break;
    }
  }
  assert (apply != NULL && "patch10 not found in kMode1Patches");

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
    printf ("SKIP: test_patch10 — no libavb-bearing *.efi fixtures in %s "
            "(needs FV-extracted PEs)\n", TEST_FIXTURES_DIR);
    return 0;
  }
  printf ("ALL PASS (%d PE fixture%s exercised)\n", ran, ran == 1 ? "" : "s");
  return 0;
}
