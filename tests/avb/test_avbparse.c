#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#define IN
#define OUT
#define OPTIONAL
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

/* Synthetic AvbVBMetaImageHeader (big-endian).
   Layout from external/avb/libavb/avb_vbmeta_image.h:
     magic[4]                       = "AVB0"
     required_libavb_version_major (u32)
     required_libavb_version_minor (u32)
     authentication_data_block_size (u64)
     auxiliary_data_block_size (u64)
     algorithm_type (u32)
     hash_offset (u64)
     hash_size (u64)
     signature_offset (u64)
     signature_size (u64)
     public_key_offset (u64)
     public_key_size (u64)
     public_key_metadata_offset (u64)
     public_key_metadata_size (u64)
     descriptors_offset (u64)
     descriptors_size (u64)
     rollback_index (u64)
     flags (u32)
     rollback_index_location (u32)
     release_string[48]
     reserved[80]
*/
static void put_u32_be (UINT8 *p, UINT32 v) {
  p[0] = (v >> 24) & 0xff; p[1] = (v >> 16) & 0xff;
  p[2] = (v >> 8) & 0xff;  p[3] = v & 0xff;
}
static void put_u64_be (UINT8 *p, UINT64 v) {
  for (int i = 0; i < 8; ++i) p[i] = (v >> (56 - i*8)) & 0xff;
}
static UINT64 get_u64_be (const UINT8 *p) {
  UINT64 v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}

static void make_vbmeta_header (UINT8 *out256,
                                 UINT64 auth_size, UINT64 aux_size,
                                 UINT32 algo, UINT64 desc_off, UINT64 desc_size,
                                 UINT32 flags) {
  memset (out256, 0, 256);
  memcpy (out256, "AVB0", 4);
  put_u32_be (out256 + 4, 1);
  put_u32_be (out256 + 8, 1);
  put_u64_be (out256 + 12, auth_size);
  put_u64_be (out256 + 20, aux_size);
  put_u32_be (out256 + 28, algo);
  put_u64_be (out256 + 32, 0);
  put_u64_be (out256 + 40, 32);
  put_u64_be (out256 + 48, 32);
  put_u64_be (out256 + 56, 256);
  put_u64_be (out256 + 64, 288);
  put_u64_be (out256 + 72, 1024);
  put_u64_be (out256 + 80, 1312);
  put_u64_be (out256 + 88, 0);
  put_u64_be (out256 + 96, desc_off);
  put_u64_be (out256 + 104, desc_size);
  put_u64_be (out256 + 112, 0);
  put_u32_be (out256 + 120, flags);
  put_u32_be (out256 + 124, 0);
}

static void test_header_parse_ok (void) {
  UINT8 region[2048];
  memset (region, 0, sizeof (region));
  make_vbmeta_header (region, 256, 512, 1, 1312, 256, 0);
  GBL_AVB_VBMETA_HEADER hdr = {0};
  EFI_STATUS s = AvbParse_VbmetaHeader (region, sizeof (region), &hdr);
  assert (s == EFI_SUCCESS);
  assert (hdr.AvbMajorVersion == 1);
  assert (hdr.AvbMinorVersion == 1);
  assert (hdr.AuthenticationDataBlockSize == 256);
  assert (hdr.AuxiliaryDataBlockSize == 512);
  assert (hdr.AlgorithmType == 1);
  assert (hdr.DescriptorsOffset == 1312);
  assert (hdr.DescriptorsSize == 256);
  printf ("ok test_header_parse_ok\n");
}

static void test_header_no_magic (void) {
  UINT8 region[256];
  memset (region, 0xCC, sizeof (region));
  GBL_AVB_VBMETA_HEADER hdr = {0};
  EFI_STATUS s = AvbParse_VbmetaHeader (region, sizeof (region), &hdr);
  assert (s == EFI_NOT_FOUND);
  printf ("ok test_header_no_magic\n");
}

static void test_header_too_small (void) {
  UINT8 region[100];
  memset (region, 0, sizeof (region));
  memcpy (region, "AVB0", 4);
  GBL_AVB_VBMETA_HEADER hdr = {0};
  EFI_STATUS s = AvbParse_VbmetaHeader (region, sizeof (region), &hdr);
  assert (s == EFI_INVALID_PARAMETER);
  printf ("ok test_header_too_small\n");
}

/* AvbDescriptor common header (big-endian):
     tag (u64)
     num_bytes_following (u64)

   AvbHashDescriptor (tag=2):
     header (16 bytes)
     image_size (u64)
     hash_algorithm[32]
     partition_name_len (u32)
     salt_len (u32)
     digest_len (u32)
     flags (u32)
     reserved[60]
     partition_name[partition_name_len]
     salt[salt_len]
     digest[digest_len]
     padding to 8-byte alignment

   AvbChainPartitionDescriptor (tag=4):
     header (16 bytes)
     rollback_index_location (u32)
     partition_name_len (u32)
     public_key_len (u32)
     reserved[64]
     partition_name[partition_name_len]
     public_key[public_key_len]
     padding
*/

static void put_desc_header (UINT8 *p, UINT64 tag, UINT64 num_bytes_following) {
  put_u64_be (p, tag);
  put_u64_be (p + 8, num_bytes_following);
}

static UINT64 align8 (UINT64 v) { return (v + 7) & ~(UINT64)7; }

static UINT64 build_hash_descriptor (UINT8 *out, CONST char *name, CONST UINT8 *digest, UINT32 digest_len) {
  UINT32 name_len = (UINT32)strlen (name);
  UINT64 body_size = 16 + 8 + 32 + 4 + 4 + 4 + 4 + 60 + name_len + 0 + digest_len;
  body_size = align8 (body_size);
  UINT64 num_bytes_following = body_size - 16;
  put_desc_header (out, 2, num_bytes_following);
  put_u64_be (out + 16, 1024);                /* image_size */
  memcpy (out + 24, "sha256", 6);             /* hash_algorithm */
  put_u32_be (out + 56, name_len);
  put_u32_be (out + 60, 0);                   /* salt_len */
  put_u32_be (out + 64, digest_len);
  put_u32_be (out + 68, 0);                   /* flags */
  /* reserved[60] at offset 72 */
  memcpy (out + 132, name, name_len);
  memcpy (out + 132 + name_len + 0, digest, digest_len);
  return body_size;
}

static UINT64 build_chain_descriptor (UINT8 *out, CONST char *name, CONST UINT8 *pk, UINT32 pk_len) {
  UINT32 name_len = (UINT32)strlen (name);
  UINT64 body_size = 16 + 4 + 4 + 4 + 64 + name_len + pk_len;
  body_size = align8 (body_size);
  UINT64 num_bytes_following = body_size - 16;
  put_desc_header (out, 4, num_bytes_following);
  put_u32_be (out + 16, 0);                   /* rollback_index_location */
  put_u32_be (out + 20, name_len);
  put_u32_be (out + 24, pk_len);
  /* reserved[64] at offset 28 */
  memcpy (out + 92, name, name_len);
  memcpy (out + 92 + name_len, pk, pk_len);
  return body_size;
}

static void test_descriptor_iterator (void) {
  UINT8 aux[2048];
  memset (aux, 0, sizeof (aux));
  UINT8 dummy_digest[32]; memset (dummy_digest, 0xDD, 32);
  UINT8 dummy_pk[64];      memset (dummy_pk, 0xEE, 64);

  UINT64 d1 = build_hash_descriptor  (aux + 0, "init_boot", dummy_digest, 32);
  UINT64 d2 = build_chain_descriptor (aux + d1, "recovery", dummy_pk, 64);
  UINT64 total = d1 + d2;

  UINT64 cursor = 0;
  GBL_AVB_DESCRIPTOR_TAG tag;
  CONST UINT8 *desc;
  UINT64 desc_len;

  EFI_STATUS s = AvbParse_NextDescriptor (aux, total, &cursor, &tag, &desc, &desc_len);
  assert (s == EFI_SUCCESS);
  assert (tag == GblAvbDescHashTag);
  assert (desc_len == d1);
  printf ("ok test_descriptor_iter first=hash\n");

  s = AvbParse_NextDescriptor (aux, total, &cursor, &tag, &desc, &desc_len);
  assert (s == EFI_SUCCESS);
  assert (tag == GblAvbDescChainPartitionTag);
  assert (desc_len == d2);
  printf ("ok test_descriptor_iter second=chain\n");

  s = AvbParse_NextDescriptor (aux, total, &cursor, &tag, &desc, &desc_len);
  assert (s == EFI_END_OF_MEDIA);
  printf ("ok test_descriptor_iter end\n");
}

static void test_parse_hash_descriptor (void) {
  UINT8 desc[256];
  memset (desc, 0, sizeof (desc));
  UINT8 digest[32]; memset (digest, 0xAB, 32);
  UINT64 dlen = build_hash_descriptor (desc, "init_boot", digest, 32);

  CONST UINT8 *name; UINT32 name_len;
  CONST UINT8 *out_digest; UINT32 out_dlen;
  EFI_STATUS s = AvbParse_HashDescriptor (desc, dlen, &name, &name_len, &out_digest, &out_dlen,
                                          NULL, NULL, NULL);
  assert (s == EFI_SUCCESS);
  assert (name_len == 9);
  assert (memcmp (name, "init_boot", 9) == 0);
  assert (out_dlen == 32);
  assert (memcmp (out_digest, digest, 32) == 0);
  printf ("ok test_parse_hash_descriptor\n");
}

static void test_parse_chain_descriptor (void) {
  UINT8 desc[256];
  memset (desc, 0, sizeof (desc));
  UINT8 pk[64]; memset (pk, 0xCD, 64);
  UINT64 dlen = build_chain_descriptor (desc, "recovery", pk, 64);

  CONST UINT8 *name; UINT32 name_len;
  CONST UINT8 *out_pk; UINT32 out_pk_len;
  EFI_STATUS s = AvbParse_ChainPartitionDescriptor (desc, dlen, &name, &name_len, &out_pk, &out_pk_len);
  assert (s == EFI_SUCCESS);
  assert (name_len == 8);
  assert (memcmp (name, "recovery", 8) == 0);
  assert (out_pk_len == 64);
  assert (memcmp (out_pk, pk, 64) == 0);
  printf ("ok test_parse_chain_descriptor\n");
}

static void test_descriptor_iter_truncated_trailer (void) {
  /* Aux block ends 8 bytes into a descriptor header — truncation, not EOS. */
  UINT8 aux[8];
  memset (aux, 0, sizeof (aux));
  UINT64 cursor = 0;
  GBL_AVB_DESCRIPTOR_TAG tag;
  CONST UINT8 *desc;
  UINT64 desc_len;
  EFI_STATUS s = AvbParse_NextDescriptor (aux, sizeof (aux), &cursor, &tag, &desc, &desc_len);
  assert (s == EFI_INVALID_PARAMETER);
  printf ("ok test_descriptor_iter_truncated_trailer\n");
}

static void test_descriptor_iter_nbf_exceeds_aux (void) {
  /* Header claims 1024-byte body but aux only has 32 bytes total. */
  UINT8 aux[32];
  memset (aux, 0, sizeof (aux));
  put_u64_be (aux, 2);                /* tag = hash */
  put_u64_be (aux + 8, 1024);         /* num_bytes_following way too large */
  UINT64 cursor = 0;
  GBL_AVB_DESCRIPTOR_TAG tag;
  CONST UINT8 *desc;
  UINT64 desc_len;
  EFI_STATUS s = AvbParse_NextDescriptor (aux, sizeof (aux), &cursor, &tag, &desc, &desc_len);
  assert (s == EFI_INVALID_PARAMETER);
  printf ("ok test_descriptor_iter_nbf_exceeds_aux\n");
}

static void test_parse_hash_descriptor_truncated_body (void) {
  /* Build a real hash descriptor, then pass a DescriptorLen smaller than 132. */
  UINT8 desc[256];
  memset (desc, 0, sizeof (desc));
  UINT8 digest[32]; memset (digest, 0xAB, 32);
  build_hash_descriptor (desc, "init_boot", digest, 32);

  CONST UINT8 *name; UINT32 name_len;
  CONST UINT8 *out_digest; UINT32 out_dlen;
  EFI_STATUS s = AvbParse_HashDescriptor (desc, 100, &name, &name_len, &out_digest, &out_dlen,
                                          NULL, NULL, NULL);
  assert (s == EFI_INVALID_PARAMETER);
  printf ("ok test_parse_hash_descriptor_truncated_body\n");
}

static void test_header_overflow_auth_plus_aux (void) {
  /* Crafted-vbmeta defense: auth_size = aux_size = 2^63. Their sum modulo 2^64
     is 0, so (256 + auth + aux) wraps to 256 — a naive `Total > VbmetaSize`
     check would silently accept any reasonably-sized buffer. The library must
     reject via overflow-safe arithmetic. */
  UINT8 region[2048];
  memset (region, 0, sizeof (region));
  make_vbmeta_header (region, 0x8000000000000000ULL, 0x8000000000000000ULL,
                      1, 0, 0, 0);
  GBL_AVB_VBMETA_HEADER hdr = {0};
  EFI_STATUS s = AvbParse_VbmetaHeader (region, sizeof (region), &hdr);
  assert (s == EFI_INVALID_PARAMETER);
  printf ("ok test_header_overflow_auth_plus_aux\n");
}

static void test_header_overflow_aux_huge (void) {
  /* auth_size fits, aux_size near UINT64_MAX. Naive code would compute
     (256 + small + huge) without overflow-safe checks and could pass. */
  UINT8 region[2048];
  memset (region, 0, sizeof (region));
  make_vbmeta_header (region, 256, 0xFFFFFFFFFFFFFE00ULL, 1, 0, 0, 0);
  GBL_AVB_VBMETA_HEADER hdr = {0};
  EFI_STATUS s = AvbParse_VbmetaHeader (region, sizeof (region), &hdr);
  assert (s == EFI_INVALID_PARAMETER);
  printf ("ok test_header_overflow_aux_huge\n");
}

static void test_parse_hash_descriptor_with_optional_outs (void) {
  UINT8 desc[256];
  memset (desc, 0, sizeof (desc));
  UINT8 digest[32]; memset (digest, 0xAB, 32);
  UINT8 salt_bytes[8] = { 0x53, 0x41, 0x4c, 0x54, 0x21, 0x21, 0x21, 0x21 };
  UINT64 dlen = build_hash_descriptor (desc, "init_boot", digest, 32);
  /* Patch salt + image_size onto build_hash_descriptor's output.
     Existing layout has name @ 132..132+9 then digest at 132+9..132+9+32 with
     salt_len=0. Insert 8 bytes of salt between name and digest. */
  put_u32_be (desc + 60, 8);                       /* salt_len = 8 */
  put_u64_be (desc + 16, 0xC0FFEE0000000123ULL);   /* image_size */
  memmove (desc + 132 + 9 + 8, desc + 132 + 9, 32); /* shift digest right */
  memcpy (desc + 132 + 9, salt_bytes, 8);
  /* Bump num_bytes_following (header u64 BE @ +8) by 8 for the inserted salt. */
  put_u64_be (desc + 8, get_u64_be (desc + 8) + 8);
  dlen += 8;

  CONST UINT8 *name; UINT32 name_len;
  CONST UINT8 *out_digest; UINT32 out_dlen;
  CONST UINT8 *out_salt; UINT32 out_salt_len;
  UINT64 out_image_size;
  EFI_STATUS s = AvbParse_HashDescriptor (desc, dlen,
                                          &name, &name_len,
                                          &out_digest, &out_dlen,
                                          &out_salt, &out_salt_len,
                                          &out_image_size);
  assert (s == EFI_SUCCESS);
  assert (name_len == 9);
  assert (out_dlen == 32);
  assert (out_salt_len == 8);
  assert (memcmp (out_salt, salt_bytes, 8) == 0);
  assert (out_image_size == 0xC0FFEE0000000123ULL);
  assert (memcmp (out_digest, digest, 32) == 0);
  printf ("ok test_parse_hash_descriptor_with_optional_outs\n");

  /* NULL-passthrough: same call with optional outs NULL must succeed and
     leave the required outs correct. */
  CONST UINT8 *name2; UINT32 name_len2;
  CONST UINT8 *digest2; UINT32 dlen2;
  s = AvbParse_HashDescriptor (desc, dlen,
                               &name2, &name_len2, &digest2, &dlen2,
                               NULL, NULL, NULL);
  assert (s == EFI_SUCCESS);
  assert (name_len2 == 9);
  assert (dlen2 == 32);
  assert (memcmp (digest2, digest, 32) == 0);
  assert (digest2 == out_digest);
  printf ("ok test_parse_hash_descriptor_with_optional_outs (NULL passthrough)\n");
}

/* --- AvbParse_FooterFromTail: decode a 64-byte footer from a partition tail
   window, bounds-checking VbmetaOffset/Size against the real partition size
   rather than the (smaller) tail buffer. Mirrors vbmeta-graft's hand-rolled
   tail decode used by probe_partition_for_graft. --- */
static void test_footer_from_tail_ok (void) {
  UINT8 tail[512];
  memset (tail, 0xAA, sizeof (tail));
  /* Partition is 1 MiB; vbmeta lives at offset 900000, size 5000 — both well
     past the 512-byte tail window, so a buffer-relative bounds check would
     wrongly reject this. */
  make_footer (tail + 512 - 64, 800000, 900000, 5000);
  GBL_AVB_FOOTER f = {0};
  EFI_STATUS s = AvbParse_FooterFromTail (tail, 512, 1048576, &f);
  assert (s == EFI_SUCCESS);
  assert (f.VbmetaOffset == 900000);
  assert (f.VbmetaSize   == 5000);
  printf ("ok test_footer_from_tail_ok\n");
}

static void test_footer_from_tail_no_magic (void) {
  UINT8 tail[128];
  memset (tail, 0, sizeof (tail));
  GBL_AVB_FOOTER f = {0};
  EFI_STATUS s = AvbParse_FooterFromTail (tail, 128, 1048576, &f);
  assert (s == EFI_NOT_FOUND);
  printf ("ok test_footer_from_tail_no_magic\n");
}

static void test_footer_from_tail_bad_bounds (void) {
  UINT8 tail[128];
  memset (tail, 0, sizeof (tail));
  /* VbmetaOffset + VbmetaSize (1048000 + 5000) overruns the 1 MiB partition. */
  make_footer (tail + 128 - 64, 100, 1048000, 5000);
  GBL_AVB_FOOTER f = {0};
  EFI_STATUS s = AvbParse_FooterFromTail (tail, 128, 1048576, &f);
  assert (s == EFI_INVALID_PARAMETER);
  printf ("ok test_footer_from_tail_bad_bounds\n");
}

/* Build a minimal embedded vbmeta blob carrying a public key in its aux block
   (auth block empty, pubkey at aux offset 0). */
static UINT64 build_embedded_vbmeta (UINT8 *out, CONST UINT8 *pk, UINT32 pk_len) {
  memset (out, 0, 256 + pk_len);
  memcpy (out, "AVB0", 4);
  put_u32_be (out + 4, 1);
  put_u32_be (out + 8, 0);
  put_u64_be (out + 12, 0);        /* authentication_data_block_size */
  put_u64_be (out + 20, pk_len);   /* auxiliary_data_block_size = pubkey only */
  put_u32_be (out + 28, 0);        /* algorithm_type */
  put_u64_be (out + 64, 0);        /* public_key_offset (within aux) */
  put_u64_be (out + 72, pk_len);   /* public_key_size */
  memcpy (out + 256, pk, pk_len);  /* aux block (auth=0, so aux starts @ 256) */
  return 256 + pk_len;
}

/* --- AvbParse_ChainVerdict: key-identity check (NOT a sig verify) of a
   chained partition's embedded vbmeta against the chain descriptor's pubkey,
   matching vbmeta-graft's GRAFT_OK / GRAFT_KEY_MISMATCH / GRAFT_NO_VBMETA. --- */
static void test_chain_verdict_ok (void) {
  UINT8 pk[64]; memset (pk, 0x11, 64);
  UINT8 blob[512];
  UINT64 n = build_embedded_vbmeta (blob, pk, 64);
  GBL_AVB_CHAIN_VERDICT v = (GBL_AVB_CHAIN_VERDICT)-1;
  EFI_STATUS s = AvbParse_ChainVerdict (blob, n, pk, 64, &v);
  assert (s == EFI_SUCCESS);
  assert (v == GblAvbChainOk);
  printf ("ok test_chain_verdict_ok\n");
}

static void test_chain_verdict_key_mismatch (void) {
  UINT8 pk[64];    memset (pk, 0x11, 64);
  UINT8 other[64]; memset (other, 0x22, 64);
  UINT8 blob[512];
  UINT64 n = build_embedded_vbmeta (blob, pk, 64);
  GBL_AVB_CHAIN_VERDICT v = (GBL_AVB_CHAIN_VERDICT)-1;
  AvbParse_ChainVerdict (blob, n, other, 64, &v);
  assert (v == GblAvbChainKeyMismatch);
  printf ("ok test_chain_verdict_key_mismatch\n");
}

static void test_chain_verdict_no_vbmeta (void) {
  UINT8 blob[512]; memset (blob, 0x77, sizeof (blob));  /* no "AVB0" magic */
  UINT8 pk[64];    memset (pk, 0x11, 64);
  GBL_AVB_CHAIN_VERDICT v = (GBL_AVB_CHAIN_VERDICT)-1;
  AvbParse_ChainVerdict (blob, sizeof (blob), pk, 64, &v);
  assert (v == GblAvbChainNoVbmeta);
  printf ("ok test_chain_verdict_no_vbmeta\n");
}

static void test_chain_verdict_null_pk (void) {
  UINT8 pk[64]; memset (pk, 0x11, 64);
  UINT8 blob[512];
  UINT64 n = build_embedded_vbmeta (blob, pk, 64);
  GBL_AVB_CHAIN_VERDICT v = (GBL_AVB_CHAIN_VERDICT)-1;
  /* No chain pubkey to compare against → any parseable vbmeta is a hit. */
  AvbParse_ChainVerdict (blob, n, NULL, 0, &v);
  assert (v == GblAvbChainOk);
  printf ("ok test_chain_verdict_null_pk\n");
}

int main (void) {
  test_footer_from_tail_ok ();
  test_footer_from_tail_no_magic ();
  test_footer_from_tail_bad_bounds ();
  test_chain_verdict_ok ();
  test_chain_verdict_key_mismatch ();
  test_chain_verdict_no_vbmeta ();
  test_chain_verdict_null_pk ();
  test_footer_parse_ok ();
  test_footer_no_magic ();
  test_footer_partition_too_small ();
  test_header_parse_ok ();
  test_header_no_magic ();
  test_header_too_small ();
  test_descriptor_iterator ();
  test_parse_hash_descriptor ();
  test_parse_chain_descriptor ();
  test_descriptor_iter_truncated_trailer ();
  test_descriptor_iter_nbf_exceeds_aux ();
  test_parse_hash_descriptor_truncated_body ();
  test_header_overflow_auth_plus_aux ();
  test_header_overflow_aux_huge ();
  test_parse_hash_descriptor_with_optional_outs ();
  printf ("ALL PASS\n");
  return 0;
}
