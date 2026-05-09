/* Host-compiled tests for ScanFor. No EDK-II framework — uses libc + assert. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

/* Provide the EDK-II type aliases ScanLib.h uses, when compiling host-side. */
#define IN
#define OUT
#define OPTIONAL
#define EFIAPI
#define STATIC static
typedef uint8_t  UINT8;
typedef uint32_t UINT32;
typedef size_t   UINTN;
typedef int      EFI_STATUS;
typedef int      BOOLEAN;
#define CONST const
#define VOID void
#define TRUE 1
#define FALSE 0

#include "../../GblChainloadPkg/Library/DynamicPatchLib/Internal/ScanLib.h"

static void test_scan_unique_match (void) {
  UINT8  Buf[64];
  UINT32 Off = 0xFFFFFFFFu;
  SCAN_RESULT R;

  memset (Buf, 0xAA, sizeof (Buf));
  Buf[20] = 0xDE; Buf[21] = 0xAD; Buf[22] = 0xBE; Buf[23] = 0xEF;
  CONST UINT8 Pat[] = { 0xDE, 0xAD, 0xBE, 0xEF };

  R = ScanFor (Buf, sizeof (Buf), Pat, NULL, sizeof (Pat), &Off);
  assert (R == SCAN_FOUND);
  assert (Off == 20);
  printf ("ok test_scan_unique_match\n");
}

static void test_scan_not_found (void) {
  UINT8  Buf[64];
  UINT32 Off = 0xFFFFFFFFu;
  SCAN_RESULT R;

  memset (Buf, 0xAA, sizeof (Buf));
  CONST UINT8 Pat[] = { 0xDE, 0xAD, 0xBE, 0xEF };

  R = ScanFor (Buf, sizeof (Buf), Pat, NULL, sizeof (Pat), &Off);
  assert (R == SCAN_NOT_FOUND);
  printf ("ok test_scan_not_found\n");
}

static void test_scan_ambiguous (void) {
  UINT8  Buf[64];
  UINT32 Off = 0xFFFFFFFFu;
  SCAN_RESULT R;

  memset (Buf, 0xAA, sizeof (Buf));
  Buf[10] = 0xDE; Buf[11] = 0xAD;
  Buf[40] = 0xDE; Buf[41] = 0xAD;
  CONST UINT8 Pat[] = { 0xDE, 0xAD };

  R = ScanFor (Buf, sizeof (Buf), Pat, NULL, sizeof (Pat), &Off);
  assert (R == SCAN_AMBIGUOUS);
  printf ("ok test_scan_ambiguous\n");
}

static void test_scan_with_mask (void) {
  UINT8  Buf[64];
  UINT32 Off = 0xFFFFFFFFu;
  SCAN_RESULT R;

  memset (Buf, 0xAA, sizeof (Buf));
  /* Match anything starting with 0xDE 0xAD followed by any 2 bytes ending 0xEF */
  Buf[30] = 0xDE; Buf[31] = 0xAD; Buf[32] = 0x12; Buf[33] = 0xEF;
  CONST UINT8 Pat[] = { 0xDE, 0xAD, 0x00, 0xEF };
  CONST UINT8 Mask[] = { 0xFF, 0xFF, 0x00, 0xFF };

  R = ScanFor (Buf, sizeof (Buf), Pat, Mask, sizeof (Pat), &Off);
  assert (R == SCAN_FOUND);
  assert (Off == 30);
  printf ("ok test_scan_with_mask\n");
}

static void test_scan_bad_input (void) {
  SCAN_RESULT R;
  UINT32 Off = 0;
  CONST UINT8 Pat[] = { 0x01 };
  R = ScanFor (NULL, 64, Pat, NULL, 1, &Off);
  assert (R == SCAN_BAD_INPUT);
  printf ("ok test_scan_bad_input\n");
}

int main (void) {
  test_scan_unique_match ();
  test_scan_not_found ();
  test_scan_ambiguous ();
  test_scan_with_mask ();
  test_scan_bad_input ();
  printf ("ALL PASS\n");
  return 0;
}
