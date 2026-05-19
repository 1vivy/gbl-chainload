/* tools/abl-patcher/abl-patcher.c — host-runnable patcher driving the same
   DynamicPatchLib code that runs on-device. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

/* The DynamicPatchLib headers — host-built via __HOST_BUILD__ in ScanLib.h. */
#include "DynamicPatchLib.h"
#include "../../GblChainloadPkg/Library/DynamicPatchLib/PatchScope.h"

static int Usage (CONST char *argv0) {
  fprintf (stderr,
    "Usage: %s --in <abl.bin> [--out <patched.bin>]\n"
    "       %s --check-anchors-only --in <abl.bin>\n"
    "       %s --oem <id> [--no-mode1] --in <abl.bin> [--out <patched.bin>]\n"
    "\n"
    "  --oem <id>         OEM patch group to apply (e.g. oneplus).\n"
    "                     Default: GBL_OEM_NONE (no OEM group; universal + mode_1).\n"
    "  --no-mode1         Exclude mode_1 patches (use for mode-2 profile ZIP).\n"
    "                     Default: mode_1 patches included.\n",
    argv0, argv0, argv0);
  return 2;
}

int main (int argc, char **argv) {
  CONST char *In         = NULL;
  CONST char *Out        = NULL;
  CONST char *OemStr     = NULL;
  int         CheckOnly  = 0;
  int         NoMode1    = 0;
  int         opt;

  static struct option longopts[] = {
    {"in",                  required_argument, 0, 'i'},
    {"out",                 required_argument, 0, 'o'},
    {"check-anchors-only",  no_argument,       0, 'c'},
    {"oem",                 required_argument, 0, 'e'},
    {"no-mode1",            no_argument,       0, 'n'},
    {"help",                no_argument,       0, 'h'},
    {0, 0, 0, 0},
  };
  while ((opt = getopt_long (argc, argv, "i:o:ce:nh", longopts, NULL)) != -1) {
    switch (opt) {
      case 'i': In       = optarg; break;
      case 'o': Out      = optarg; break;
      case 'c': CheckOnly = 1;    break;
      case 'e': OemStr   = optarg; break;
      case 'n': NoMode1  = 1;     break;
      case 'h': default: return Usage (argv[0]);
    }
  }

  if (!In) return Usage (argv[0]);

  /* Resolve OEM id and call the appropriate init path.
     Default is GBL_OEM_NONE — EnsureInitScoped skips all OEM-group patches.
     Previously plain invocation implicitly aggregated the OnePlus OEM patches;
     that is no longer the case.  Callers needing OEM patches (e.g. the install
     ZIP, if it wants OEM patches applied) must pass --oem <id> explicitly. */
  GBL_OEM Oem = GBL_OEM_NONE;
  if (OemStr != NULL) {
    if (strcmp (OemStr, "oneplus") == 0) {
      Oem = GBL_OEM_ONEPLUS;
    } else {
      fprintf (stderr, "error: unknown --oem '%s'\n", OemStr);
      return 2;
    }
  }

  /* include_mode1: on by default; --no-mode1 disables it.
     Plain invocation (no --oem, no --no-mode1) preserves the old behaviour:
     universal + oneplus(skip) + mode_1. */
  int IncludeMode1 = !NoMode1;
  DynamicPatchLib_EnsureInitScoped (Oem, IncludeMode1);

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
