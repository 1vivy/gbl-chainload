/* tools/abl-patcher/abl-patcher.c — host-runnable patcher driving the same
   DynamicPatchLib code that runs on-device. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

/* The DynamicPatchLib headers — host-built via __HOST_BUILD__ in ScanLib.h. */
#include "DynamicPatchLib.h"

VOID DynamicPatchLib_EnsureInit (VOID);

static int Usage (CONST char *argv0) {
  fprintf (stderr,
    "Usage: %s --in <abl.bin> [--out <patched.bin>]\n"
    "       %s --check-anchors-only --in <abl.bin>\n",
    argv0, argv0);
  return 2;
}

int main (int argc, char **argv) {
  CONST char *In         = NULL;
  CONST char *Out        = NULL;
  int         CheckOnly  = 0;
  int         opt;

  static struct option longopts[] = {
    {"in",                  required_argument, 0, 'i'},
    {"out",                 required_argument, 0, 'o'},
    {"check-anchors-only",  no_argument,       0, 'c'},
    {"help",                no_argument,       0, 'h'},
    {0, 0, 0, 0},
  };
  while ((opt = getopt_long (argc, argv, "i:o:ch", longopts, NULL)) != -1) {
    switch (opt) {
      case 'i': In = optarg; break;
      case 'o': Out = optarg; break;
      case 'c': CheckOnly = 1; break;
      case 'h': default: return Usage (argv[0]);
    }
  }

  if (!In) return Usage (argv[0]);

  /* Load file. */
  FILE *f = fopen (In, "rb");
  if (!f) { perror (In); return 1; }
  fseek (f, 0, SEEK_END);
  long Sz = ftell (f);
  fseek (f, 0, SEEK_SET);
  if (Sz <= 0) { fprintf (stderr, "%s: empty file\n", In); fclose (f); return 1; }
  UINT8 *Buf = (UINT8 *)malloc ((size_t)Sz);
  if (!Buf) { fprintf (stderr, "OOM\n"); fclose (f); return 1; }
  if (fread (Buf, 1, (size_t)Sz, f) != (size_t)Sz) {
    fprintf (stderr, "%s: read failed\n", In); fclose (f); free (Buf); return 1;
  }
  fclose (f);

  DynamicPatchLib_EnsureInit ();
  PATCH_RESULT R = {0};
  DynamicPatch_Apply (Buf, (UINT32)Sz, &R);

  fprintf (stderr,
    "%s: applied=%u missed=%u worst=%d (0=ok 1=optional-miss 2=mandatory-miss)\n",
    In, R.AppliedCount, R.MissedCount, (int)R.WorstOutcome);

  if (CheckOnly) {
    /* Anchor-uniqueness mode: any PATCH_AMBIGUOUS would cause MissedCount > 0.
       For check-only, we want ambiguity to be a hard error.  However the engine
       lumps AMBIGUOUS and MISS into a single MissedCount.  As a coarse signal,
       fail only if there's a mandatory miss (which includes mandatory ambiguous).
       Future enhancement: add per-patch outcome reporting via a callback so
       check-anchors-only can specifically report ambiguity. */
    if (R.WorstOutcome == PATCH_RESULT_MANDATORY_MISS) {
      fprintf (stderr, "FAIL: mandatory patch missed/ambiguous on %s\n", In);
      free (Buf);
      return 1;
    }
    fprintf (stderr, "ok check-anchors-only on %s\n", In);
    free (Buf);
    return 0;
  }

  /* Apply mode (default): write patched output if --out given, fail on
     mandatory miss. */
  if (R.WorstOutcome == PATCH_RESULT_MANDATORY_MISS) {
    fprintf (stderr, "ERROR: mandatory patch missed on %s\n", In);
    free (Buf);
    return 1;
  }

  if (Out) {
    FILE *o = fopen (Out, "wb");
    if (!o) { perror (Out); free (Buf); return 1; }
    if (fwrite (Buf, 1, (size_t)Sz, o) != (size_t)Sz) {
      fprintf (stderr, "%s: write failed\n", Out); fclose (o); free (Buf); return 1;
    }
    fclose (o);
    fprintf (stderr, "wrote %ld bytes to %s\n", Sz, Out);
  }

  free (Buf);
  return 0;
}
