/* Host test for patch1 (efisp recursion) — RETIRED.
 *
 * patch1 was retired from the active patch table in Task 10 (engine-rework).
 * The BlockIoHook EFISP gate is the operational replacement and is exercised
 * by its own hook-level tests.  This file is kept as a placeholder so the
 * test_patch1 binary still exists for run-all wiring, but it does nothing
 * except emit a SKIP line.
 *
 * The retired implementation lives at
 *   GblChainloadPkg/Library/DynamicPatchLib/retired/block_efisp_recursion.c
 * and can be re-imported with a real test driver if patch1 is ever revived.
 */

#include <stdio.h>

int
main (void)
{
  printf ("SKIP: test_patch1 — patch1-efisp-recursion retired "
          "(engine-rework Task 10); BlockIoHook EFISP gate covers this case\n");
  return 0;
}
