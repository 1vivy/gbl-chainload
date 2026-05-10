#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#define IN
#define OUT
#define EFIAPI
#define STATIC static
#define CONST const
typedef uint8_t  UINT8;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef char     CHAR8;
typedef int      EFI_STATUS;
#define EFI_SUCCESS              0
#define EFI_NOT_FOUND            14
#define EFI_INVALID_PARAMETER    2
#define EFI_END_OF_MEDIA         28
#define EFI_ERROR(s)  ((s) != 0)

#include "../../GblChainloadPkg/Include/Library/AvbParseLib.h"

static void make_footer (UINT8 *footer64, UINT64 orig_size, UINT64 vbm_off, UINT64 vbm_sz) {
  memset (footer64, 0, 64);
  memcpy (footer64, "AVBf", 4);
  footer64[4]=0; footer64[5]=0; footer64[6]=0; footer64[7]=1;
  footer64[8]=0; footer64[9]=0; footer64[10]=0; footer64[11]=0;
  for (int i = 0; i < 8; ++i) footer64[12+i] = (orig_size >> (56 - i*8)) & 0xff;
  for (int i = 0; i < 8; ++i) footer64[20+i] = (vbm_off  >> (56 - i*8)) & 0xff;
  for (int i = 0; i < 8; ++i) footer64[28+i] = (vbm_sz   >> (56 - i*8)) & 0xff;
}

static void test_footer_parse_ok (void) {
  UINT8 partition[1024];
  memset (partition, 0xAA, sizeof (partition));
  make_footer (partition + 1024 - 64, 512, 300, 200);
  GBL_AVB_FOOTER footer = {0};
  EFI_STATUS s = AvbParse_Footer (partition, 1024, &footer);
  assert (s == EFI_SUCCESS);
  assert (footer.FooterMajorVersion == 1);
  assert (footer.OriginalImageSize  == 512);
  assert (footer.VbmetaOffset       == 300);
  assert (footer.VbmetaSize         == 200);
  printf ("ok test_footer_parse_ok\n");
}

static void test_footer_no_magic (void) {
  UINT8 partition[1024];
  memset (partition, 0xAA, sizeof (partition));
  GBL_AVB_FOOTER footer = {0};
  EFI_STATUS s = AvbParse_Footer (partition, 1024, &footer);
  assert (s == EFI_NOT_FOUND);
  printf ("ok test_footer_no_magic\n");
}

static void test_footer_partition_too_small (void) {
  UINT8 partition[32];
  GBL_AVB_FOOTER footer = {0};
  EFI_STATUS s = AvbParse_Footer (partition, 32, &footer);
  assert (s == EFI_INVALID_PARAMETER);
  printf ("ok test_footer_partition_too_small\n");
}

int main (void) {
  test_footer_parse_ok ();
  test_footer_no_magic ();
  test_footer_partition_too_small ();
  printf ("ALL PASS\n");
  return 0;
}
