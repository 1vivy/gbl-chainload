#include <stdio.h>
#include <assert.h>
#include "../../GblChainloadPkg/Include/Library/DynamicPatchLib.h"

extern CONST PATCH_DESC  *gPatchTable;
extern UINTN              gPatchTableLen;

static int g_apply_count = 0;
static PATCH_OUTCOME StubOk    (UINT8 *b, UINT32 s) { (void)b; (void)s; ++g_apply_count; return PATCH_OK; }
static PATCH_OUTCOME StubMiss  (UINT8 *b, UINT32 s) { (void)b; (void)s; ++g_apply_count; return PATCH_MISS; }
static PATCH_OUTCOME StubAmbig (UINT8 *b, UINT32 s) { (void)b; (void)s; ++g_apply_count; return PATCH_AMBIGUOUS; }

static void test_engine_all_ok (void) {
  static CONST PATCH_DESC table[] = {
    { "p1", SCOPE_UNIVERSAL, TRUE,  StubOk },
    { "p2", SCOPE_UNIVERSAL, FALSE, StubOk },
  };
  gPatchTable    = table;
  gPatchTableLen = sizeof (table) / sizeof (table[0]);

  UINT8 buf[16] = {0};
  PATCH_RESULT R = {0};
  g_apply_count = 0;
  DynamicPatch_Apply (buf, sizeof (buf), &R);
  assert (g_apply_count == 2);
  assert (R.AppliedCount == 2);
  assert (R.MissedCount  == 0);
  assert (R.WorstOutcome == PATCH_RESULT_OK);
  printf ("ok test_engine_all_ok\n");
}

static void test_engine_optional_miss (void) {
  static CONST PATCH_DESC table[] = {
    { "p1", SCOPE_UNIVERSAL,   TRUE,  StubOk   },
    { "p2", SCOPE_OEM_OPLUS, FALSE, StubMiss },  /* optional miss */
  };
  gPatchTable    = table;
  gPatchTableLen = 2;

  UINT8 buf[16] = {0};
  PATCH_RESULT R = {0};
  DynamicPatch_Apply (buf, sizeof (buf), &R);
  assert (R.AppliedCount == 1);
  assert (R.MissedCount  == 1);
  assert (R.WorstOutcome == PATCH_RESULT_OPTIONAL_MISS);
  printf ("ok test_engine_optional_miss\n");
}

static void test_engine_mandatory_miss (void) {
  static CONST PATCH_DESC table[] = {
    { "p1", SCOPE_UNIVERSAL, TRUE,  StubMiss },   /* mandatory miss */
    { "p2", SCOPE_UNIVERSAL, FALSE, StubOk   },
  };
  gPatchTable    = table;
  gPatchTableLen = 2;

  UINT8 buf[16] = {0};
  PATCH_RESULT R = {0};
  DynamicPatch_Apply (buf, sizeof (buf), &R);
  assert (R.AppliedCount == 1);
  assert (R.MissedCount  == 1);
  assert (R.WorstOutcome == PATCH_RESULT_MANDATORY_MISS);
  printf ("ok test_engine_mandatory_miss\n");
}

static void test_engine_ambiguous_counts_as_miss (void) {
  /* PATCH_AMBIGUOUS is not OK; treat as miss for result tracking. */
  static CONST PATCH_DESC table[] = {
    { "p1", SCOPE_UNIVERSAL, TRUE, StubAmbig },
  };
  gPatchTable    = table;
  gPatchTableLen = 1;

  UINT8 buf[16] = {0};
  PATCH_RESULT R = {0};
  DynamicPatch_Apply (buf, sizeof (buf), &R);
  assert (R.AppliedCount == 0);
  assert (R.MissedCount  == 1);
  assert (R.WorstOutcome == PATCH_RESULT_MANDATORY_MISS);
  printf ("ok test_engine_ambiguous_counts_as_miss\n");
}

static void test_engine_empty_table (void) {
  gPatchTable    = NULL;
  gPatchTableLen = 0;

  UINT8 buf[16] = {0};
  PATCH_RESULT R = {0};
  DynamicPatch_Apply (buf, sizeof (buf), &R);
  assert (R.AppliedCount == 0);
  assert (R.MissedCount  == 0);
  assert (R.WorstOutcome == PATCH_RESULT_OK);
  printf ("ok test_engine_empty_table\n");
}

int main (void) {
  test_engine_all_ok ();
  test_engine_optional_miss ();
  test_engine_mandatory_miss ();
  test_engine_ambiguous_counts_as_miss ();
  test_engine_empty_table ();
  printf ("ALL PASS\n");
  return 0;
}
