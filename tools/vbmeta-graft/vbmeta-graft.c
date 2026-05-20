/* tools/vbmeta-graft/vbmeta-graft.c — list / check / graft AVB vbmeta.
 *
 *   vbmeta-graft list      <vbmeta-or-partition-img>
 *   vbmeta-graft check     <candidate-partition-img> <main-vbmeta-img> <part>
 *   vbmeta-graft graft     --stock <stock-part-img> --custom <custom-img>
 *                          --part-size <bytes> --out <grafted-img>
 *   vbmeta-graft list-hash <active-vbmeta-img> <byname-dir>
 *
 * Reuses GblChainloadPkg/Library/AvbParseLib for AVB structure parsing
 * (compiled with -D__HOST_BUILD__; the Makefile builds AvbParse.c too).
 *
 * AvbBigEndian.h (internal) defines all EDK2 type shims when __HOST_BUILD__
 * is set. Include it before AvbParseLib.h so the public header's UINT8/
 * UINT32/UINT64/EFI_STATUS etc. resolve. The Makefile sets -I$(AVB)/Internal.
 *
 * _POSIX_C_SOURCE: expose fileno() and fstat() under -std=c11.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

#include "Sha256.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "AvbBigEndian.h"
#pragma GCC diagnostic pop
#include "../../GblChainloadPkg/Include/Library/AvbParseLib.h"

/* slurp: read a whole file into a malloc'd buffer. */
static uint8_t *slurp(const char *path, size_t *len_out)
{
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "vbmeta-graft: %s: cannot open\n", path); return NULL; }
  struct stat st;
  if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
    fprintf(stderr, "vbmeta-graft: %s: not a regular file\n", path);
    fclose(f); return NULL;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fprintf(stderr, "vbmeta-graft: %s: fseek error\n", path);
    fclose(f); return NULL;
  }
  long n = ftell(f);
  if (fseek(f, 0, SEEK_SET) != 0) {
    fprintf(stderr, "vbmeta-graft: %s: fseek error\n", path);
    fclose(f); return NULL;
  }
  if (n <= 0) { fprintf(stderr, "vbmeta-graft: %s: empty\n", path); fclose(f); return NULL; }
  uint8_t *buf = malloc((size_t)n);
  if (!buf) { fclose(f); return NULL; }
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
    fprintf(stderr, "vbmeta-graft: %s: read error\n", path);
    free(buf); fclose(f); return NULL;
  }
  fclose(f);
  *len_out = (size_t)n;
  return buf;
}

/* locate_vbmeta: point at the vbmeta blob inside a buffer. If the buffer
 * has an AvbFooter (footer'd partition), use it; else treat the whole
 * buffer as a bare vbmeta blob (e.g. the main `vbmeta` partition). */
static int locate_vbmeta(const uint8_t *buf, size_t len,
                         const uint8_t **vb_out, uint64_t *vb_len_out)
{
  GBL_AVB_FOOTER footer;
  if (AvbParse_Footer(buf, (UINT64)len, &footer) == EFI_SUCCESS) {
    if (footer.VbmetaOffset > len ||
        footer.VbmetaSize  > len - footer.VbmetaOffset) {
      return -1;
    }
    *vb_out     = buf + footer.VbmetaOffset;
    *vb_len_out = footer.VbmetaSize;
    return 0;
  }
  /* No footer: the buffer itself should start with the vbmeta magic. */
  if (len >= 4 && memcmp(buf, GBL_AVB_VBMETA_MAGIC, 4) == 0) {
    *vb_out     = buf;
    *vb_len_out = (uint64_t)len;
    return 0;
  }
  return -1;
}

/* aux_block: compute the auxiliary block pointer + size from a header. */
static const uint8_t *aux_block(const uint8_t *vb, const GBL_AVB_VBMETA_HEADER *h,
                                uint64_t *aux_len_out)
{
  *aux_len_out = h->AuxiliaryDataBlockSize;
  return vb + GBL_AVB_VBMETA_HEADER_SIZE + h->AuthenticationDataBlockSize;
}

/* descriptor_walk callback type. */
typedef void (*desc_fn)(GBL_AVB_DESCRIPTOR_TAG tag, const uint8_t *desc,
                        uint64_t desc_len, void *ctx);

/* walk every descriptor of a vbmeta blob. Returns 0 on success. */
static int walk_descriptors(const uint8_t *vb, uint64_t vb_len,
                            desc_fn fn, void *ctx)
{
  GBL_AVB_VBMETA_HEADER h;
  if (AvbParse_VbmetaHeader(vb, vb_len, &h) != EFI_SUCCESS) return -1;
  uint64_t aux_len;
  const uint8_t *aux = aux_block(vb, &h, &aux_len);
  uint64_t cursor = h.DescriptorsOffset;
  uint64_t end    = h.DescriptorsOffset + h.DescriptorsSize;
  while (cursor < end) {
    GBL_AVB_DESCRIPTOR_TAG tag;
    const uint8_t *desc;
    uint64_t desc_len;
    if (AvbParse_NextDescriptor(aux, aux_len, &cursor, &tag, &desc, &desc_len)
        != EFI_SUCCESS)
      break;
    fn(tag, desc, desc_len, ctx);
  }
  return 0;
}

/* ---- list ----------------------------------------------------------- */

static void list_cb(GBL_AVB_DESCRIPTOR_TAG tag, const uint8_t *desc,
                    uint64_t desc_len, void *ctx)
{
  (void)ctx;
  const char *kind = "other";
  const uint8_t *name = NULL;
  uint32_t name_len = 0;
  if (tag == GblAvbDescHashTag) {
    kind = "hash";
    const uint8_t *digest;
    uint32_t digest_len;
    AvbParse_HashDescriptor(desc, desc_len, &name, &name_len, &digest, &digest_len);
  } else if (tag == GblAvbDescChainPartitionTag) {
    kind = "chain";
    const uint8_t *pk;
    uint32_t pk_len;
    AvbParse_ChainPartitionDescriptor(desc, desc_len, &name, &name_len, &pk, &pk_len);
  } else if (tag == GblAvbDescHashtreeTag) {
    kind = "hashtree";
  }
  if (name && name_len) {
    printf("partition=%.*s type=%s graftable=%s\n",
           (int)name_len, (const char *)name, kind,
           (tag == GblAvbDescHashTag || tag == GblAvbDescChainPartitionTag)
             ? "yes" : "no");
  } else {
    printf("descriptor type=%s\n", kind);
  }
}

static int cmd_list(const char *path)
{
  size_t len;
  uint8_t *buf = slurp(path, &len);
  if (!buf) return 1;
  const uint8_t *vb;
  uint64_t vb_len;
  if (locate_vbmeta(buf, len, &vb, &vb_len) != 0) {
    fprintf(stderr, "vbmeta-graft: %s: no vbmeta found\n", path);
    free(buf); return 1;
  }
  int rc = walk_descriptors(vb, vb_len, list_cb, NULL);
  free(buf);
  return rc == 0 ? 0 : 1;
}

/* ---- check ---------------------------------------------------------- */

/* find_chain_pubkey: locate <part>'s chain descriptor in a main vbmeta and
 * copy its public key into a malloc'd buffer. Returns NULL if not found. */
struct chain_ctx { const char *part; uint8_t *pk; uint32_t pk_len; };

static void chain_cb(GBL_AVB_DESCRIPTOR_TAG tag, const uint8_t *desc,
                     uint64_t desc_len, void *vctx)
{
  struct chain_ctx *c = vctx;
  if (c->pk || tag != GblAvbDescChainPartitionTag) return;
  const uint8_t *name;
  uint32_t name_len;
  const uint8_t *pk;
  uint32_t pk_len;
  if (AvbParse_ChainPartitionDescriptor(desc, desc_len, &name, &name_len,
                                        &pk, &pk_len) != EFI_SUCCESS)
    return;
  if (name_len == (uint32_t)strlen(c->part) &&
      memcmp(name, c->part, name_len) == 0) {
    c->pk = malloc(pk_len);
    if (c->pk) { memcpy(c->pk, pk, pk_len); c->pk_len = pk_len; }
  }
}

static int cmd_check(const char *cand_path, const char *main_path,
                     const char *part)
{
  size_t cl, ml;
  uint8_t *cand = slurp(cand_path, &cl);
  if (!cand) return 1;
  uint8_t *mainb = slurp(main_path, &ml);
  if (!mainb) { free(cand); return 1; }

  const uint8_t *cvb; uint64_t cvb_len;
  const uint8_t *mvb; uint64_t mvb_len;
  if (locate_vbmeta(cand, cl, &cvb, &cvb_len) != 0 ||
      locate_vbmeta(mainb, ml, &mvb, &mvb_len) != 0) {
    fprintf(stderr, "vbmeta-graft: check: unparseable vbmeta\n");
    free(cand); free(mainb); return 1;
  }

  /* candidate's own public key (header offsets into its aux block) */
  GBL_AVB_VBMETA_HEADER ch;
  if (AvbParse_VbmetaHeader(cvb, cvb_len, &ch) != EFI_SUCCESS) {
    fprintf(stderr, "vbmeta-graft: check: bad candidate vbmeta\n");
    free(cand); free(mainb); return 1;
  }
  uint64_t caux_len;
  const uint8_t *caux = aux_block(cvb, &ch, &caux_len);
  if (ch.PublicKeyOffset > caux_len ||
      ch.PublicKeySize  > caux_len - ch.PublicKeyOffset) {
    fprintf(stderr, "vbmeta-graft: check: candidate public key out of bounds\n");
    free(cand); free(mainb); return 1;
  }
  const uint8_t *cand_pk = caux + ch.PublicKeyOffset;
  uint32_t cand_pk_len = (uint32_t)ch.PublicKeySize;
  printf("rollback-index: %llu\n", (unsigned long long)ch.RollbackIndex);

  /* the key <part>'s chain descriptor in the main vbmeta names */
  struct chain_ctx cc = { part, NULL, 0 };
  walk_descriptors(mvb, mvb_len, chain_cb, &cc);
  int rc;
  if (!cc.pk) {
    fprintf(stderr, "vbmeta-graft: check: no chain descriptor for '%s'\n", part);
    rc = 2;                      /* parsed, but unsuitable */
  } else if (cc.pk_len == cand_pk_len &&
             memcmp(cc.pk, cand_pk, cand_pk_len) == 0) {
    printf("suitable: key matches chain descriptor for %s\n", part);
    rc = 0;
  } else {
    fprintf(stderr, "vbmeta-graft: check: key mismatch for '%s'\n", part);
    rc = 2;
  }
  free(cc.pk); free(cand); free(mainb);
  return rc;
}

/* ---- graft ---------------------------------------------------------- */

static void put_u32_be(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)((v >> 24) & 0xff);
  p[1] = (uint8_t)((v >> 16) & 0xff);
  p[2] = (uint8_t)((v >>  8) & 0xff);
  p[3] = (uint8_t)( v        & 0xff);
}

static void put_u64_be(uint8_t *p, uint64_t v)
{
  int i;
  for (i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (56 - i * 8));
}

static int cmd_graft(const char *stock_path, const char *custom_path,
                     uint64_t part_size, const char *out_path)
{
  size_t sl, custl;
  uint8_t *stock = slurp(stock_path, &sl);
  if (!stock) return 1;
  uint8_t *custom = slurp(custom_path, &custl);
  if (!custom) { free(stock); return 1; }

  const uint8_t *svb;
  uint64_t svb_len;
  if (locate_vbmeta(stock, sl, &svb, &svb_len) != 0) {
    fprintf(stderr, "vbmeta-graft: graft: no stock vbmeta\n");
    free(stock); free(custom); return 1;
  }

  /* The custom image's content size is its own AvbFooter's
     OriginalImageSize when it is a footer'd / partition-sized image (a
     recovery partition dump or a built recovery.img); only a bare,
     unpadded custom image has content size == file size. Using the file
     size for a footer'd image would leave no room for the grafted vbmeta. */
  uint64_t content;
  GBL_AVB_FOOTER cust_footer;
  if (AvbParse_Footer(custom, (UINT64)custl, &cust_footer) == EFI_SUCCESS) {
    content = cust_footer.OriginalImageSize;
  } else {
    content = (uint64_t)custl;
  }
  uint64_t vb_off    = (content + 4095) & ~(uint64_t)4095;   /* round up 4K */
  uint64_t footer_at = part_size - GBL_AVB_FOOTER_SIZE;
  if (vb_off + svb_len > footer_at) {
    fprintf(stderr, "vbmeta-graft: graft: custom image too large for the "
                    "partition (%llu B content + vbmeta exceeds %llu B)\n",
            (unsigned long long)content, (unsigned long long)part_size);
    free(stock); free(custom); return 1;
  }

  uint8_t *img = calloc(1, (size_t)part_size);
  if (!img) { free(stock); free(custom); return 1; }
  memcpy(img, custom, (size_t)content);               /* custom content at 0    */
  memcpy(img + vb_off, svb, (size_t)svb_len);         /* stock vbmeta blob      */

  uint8_t *ft = img + footer_at;                      /* 64-byte AvbFooter      */
  memcpy(ft, GBL_AVB_FOOTER_MAGIC, 4);
  put_u32_be(ft +  4, 1);                             /* footer major version  \
                                                         1.0 = AVB footer spec  \
                                                         version this tool       \
                                                         targets (not copied     \
                                                         from stock footer)      */
  put_u32_be(ft +  8, 0);                             /* footer minor version  */
  put_u64_be(ft + 12, content);                       /* OriginalImageSize     */
  put_u64_be(ft + 20, vb_off);                        /* VbmetaOffset          */
  put_u64_be(ft + 28, svb_len);                       /* VbmetaSize            */

  FILE *o = fopen(out_path, "wb");
  if (!o) {
    fprintf(stderr, "vbmeta-graft: %s: cannot write\n", out_path);
    free(img); free(stock); free(custom); return 1;
  }
  int ok = (fwrite(img, 1, (size_t)part_size, o) == (size_t)part_size);
  fclose(o);
  free(img); free(stock); free(custom);
  if (!ok) { fprintf(stderr, "vbmeta-graft: graft: short write\n"); return 1; }
  fprintf(stderr, "vbmeta-graft: grafted %s (%llu B, vbmeta @ 0x%llx)\n",
          out_path, (unsigned long long)part_size, (unsigned long long)vb_off);
  return 0;
}

/* ---- list-hash ------------------------------------------------------ */

/*
 * AVB hash descriptor raw offsets (libavb avb_hash_descriptor.h):
 *   0   tag (u64 BE)
 *   8   num_bytes_following (u64 BE)
 *  16   image_size (u64 BE)              <-- AvbDescriptor(16) + image_size
 *  24   hash_algorithm (char[32])
 *  56   partition_name_len (u32 BE)      <- confirmed by AvbParse.c and hex dump
 *  60   salt_len (u32 BE)
 *  64   digest_len (u32 BE)
 *  68   flags (u32 BE)
 *  72   reserved[60]
 * 132   variable: name || salt || digest
 *
 * AvbParse_HashDescriptor only extracts name and digest; we read image_size
 * and salt manually from the raw descriptor bytes.
 */

/* Derive slot suffix: env GBL_VBMETA_SLOT > tail-match _a/_b on path > "a" */
static const char *derive_slot(const char *mvb_path)
{
  const char *env = getenv("GBL_VBMETA_SLOT");
  if (env && (strcmp(env, "a") == 0 || strcmp(env, "b") == 0))
    return env;

  /* Check basename for _a or _b suffix */
  const char *base = strrchr(mvb_path, '/');
  base = base ? base + 1 : mvb_path;
  size_t blen = strlen(base);
  if (blen >= 2 && base[blen-2] == '_') {
    if (base[blen-1] == 'a') return "a";
    if (base[blen-1] == 'b') return "b";
  }

  fprintf(stderr, "note: slot suffix defaulted to 'a'\n");
  return "a";
}

/* Scan buf[0..len) for an AVB0 magic; return pointer to first hit or NULL. */
static const uint8_t *find_avb0(const uint8_t *buf, size_t len)
{
  if (len < 4) return NULL;
  for (size_t i = 0; i + 4 <= len; i++) {
    if (buf[i]=='A' && buf[i+1]=='V' && buf[i+2]=='B' && buf[i+3]=='0')
      return buf + i;
  }
  return NULL;
}

/* Probe partition buffer for a valid OEM-signed vbmeta blob whose embedded
 * public key matches chain_pk/chain_pk_len. Returns 1 if found, 0 if not. */
static int probe_graft(const uint8_t *part, size_t part_len,
                       const uint8_t *chain_pk, uint32_t chain_pk_len)
{
  /* Walk looking for AVB0 magic. For each hit, parse the vbmeta header and
   * compare its embedded public key to the chain descriptor's public key. */
  const uint8_t *p = part;
  size_t rem = part_len;
  while (rem >= 4) {
    const uint8_t *hit = find_avb0(p, rem);
    if (!hit) break;
    size_t off = (size_t)(hit - part);
    size_t avail = part_len - off;
    GBL_AVB_VBMETA_HEADER vh;
    if (AvbParse_VbmetaHeader(hit, (uint64_t)avail, &vh) == EFI_SUCCESS) {
      /* Get embedded public key */
      uint64_t aux_len;
      const uint8_t *aux = aux_block(hit, &vh, &aux_len);
      if (vh.PublicKeyOffset <= aux_len &&
          vh.PublicKeySize   <= aux_len - vh.PublicKeyOffset) {
        const uint8_t *pk = aux + vh.PublicKeyOffset;
        uint32_t pk_len   = (uint32_t)vh.PublicKeySize;
        if (chain_pk && chain_pk_len > 0) {
          if (pk_len == chain_pk_len && memcmp(pk, chain_pk, pk_len) == 0)
            return 1;
        } else {
          /* No chain key available: any valid vbmeta header counts */
          return 1;
        }
      }
    }
    /* Advance past this hit and keep searching */
    p = hit + 4;
    rem = part_len - (size_t)(p - part);
  }
  return 0;
}

struct lh_ctx {
  const char *byname_dir;
  const char *slot;
};

static void lh_cb(GBL_AVB_DESCRIPTOR_TAG tag, const uint8_t *desc,
                  uint64_t desc_len, void *vctx)
{
  struct lh_ctx *ctx = vctx;

  if (tag == GblAvbDescHashTag) {
    /* --- hash descriptor --- */
    const uint8_t *name = NULL;
    uint32_t name_len = 0;
    const uint8_t *digest = NULL;
    uint32_t digest_len = 0;
    if (AvbParse_HashDescriptor(desc, desc_len, &name, &name_len,
                                &digest, &digest_len) != EFI_SUCCESS)
      return;

    /* Read image_size and salt from raw descriptor bytes */
    if (desc_len < 132) return;
    uint64_t image_size = 0;
    for (int i = 0; i < 8; i++)
      image_size = (image_size << 8) | desc[16 + i];  /* image_size at offset 16 */
    uint32_t salt_len = (uint32_t)((desc[60]<<24)|(desc[61]<<16)|(desc[62]<<8)|desc[63]);
    const uint8_t *salt = desc + 132 + name_len;

    /* Build partition path */
    char path[4096];
    snprintf(path, sizeof(path), "%s/%.*s_%s",
             ctx->byname_dir, (int)name_len, (const char *)name, ctx->slot);

    /* Compute: SHA-256(salt || partition_bytes[0..image_size)) */
    const char *digest_status = "missing";
    const char *verdict = "mismatch";
    uint8_t *part_buf = NULL;
    size_t part_len = 0;

    part_buf = slurp(path, &part_len);
    if (part_buf) {
      uint64_t read_size = image_size;
      if (read_size > (uint64_t)part_len)
        read_size = (uint64_t)part_len;

      /* SHA-256(salt || content) — two-pass: compute via concatenated input */
      /* Use a streaming approach: init context, feed salt, feed image bytes */
      /* gbl_sha256 is single-shot; allocate a combined buffer */
      uint64_t total = (uint64_t)salt_len + read_size;
      if (total <= 128 * 1024 * 1024ULL) { /* sanity: <= 128 MiB */
        uint8_t *combined = malloc((size_t)total);
        if (combined) {
          if (salt_len > 0)
            memcpy(combined, salt, salt_len);
          memcpy(combined + salt_len, part_buf, (size_t)read_size);
          uint8_t got[32];
          gbl_sha256(combined, (size_t)total, got);
          free(combined);
          if (digest_len == 32 && memcmp(got, digest, 32) == 0) {
            digest_status = "ok";
            verdict = "match";
          } else {
            digest_status = "mismatch";
            verdict = "mismatch";
          }
        }
      }
      free(part_buf);
    }

    printf("partition=%-16.*s type=hash  declared=%-12llu digest=%-8s graft=n/a   verdict=%s\n",
           (int)name_len, (const char *)name,
           (unsigned long long)image_size,
           digest_status, verdict);

  } else if (tag == GblAvbDescChainPartitionTag) {
    /* --- chain descriptor --- */
    const uint8_t *name = NULL;
    uint32_t name_len = 0;
    const uint8_t *chain_pk = NULL;
    uint32_t chain_pk_len = 0;
    if (AvbParse_ChainPartitionDescriptor(desc, desc_len, &name, &name_len,
                                          &chain_pk, &chain_pk_len) != EFI_SUCCESS)
      return;

    /* Build partition path */
    char path[4096];
    snprintf(path, sizeof(path), "%s/%.*s_%s",
             ctx->byname_dir, (int)name_len, (const char *)name, ctx->slot);

    const char *graft_status = "missing";
    const char *verdict = "mismatch";
    size_t part_len = 0;
    uint8_t *part_buf = slurp(path, &part_len);
    if (part_buf) {
      if (probe_graft(part_buf, part_len, chain_pk, chain_pk_len)) {
        graft_status = "ok";
        verdict = "match";
      }
      free(part_buf);
    }

    printf("partition=%-16.*s type=chain declared=%-12s digest=n/a  graft=%-7s verdict=%s\n",
           (int)name_len, (const char *)name,
           "-", graft_status, verdict);
  }
}

static int cmd_list_hash(const char *mvb_path, const char *byname_dir)
{
  size_t len;
  uint8_t *buf = slurp(mvb_path, &len);
  if (!buf) return 1;

  const uint8_t *vb;
  uint64_t vb_len;
  if (locate_vbmeta(buf, len, &vb, &vb_len) != 0) {
    fprintf(stderr, "vbmeta-graft: %s: no vbmeta found\n", mvb_path);
    free(buf); return 1;
  }

  const char *slot = derive_slot(mvb_path);
  struct lh_ctx ctx = { byname_dir, slot };
  int rc = walk_descriptors(vb, vb_len, lh_cb, &ctx);
  free(buf);
  return rc == 0 ? 0 : 1;
}

/* ---- main ----------------------------------------------------------- */

static int usage(void)
{
  fprintf(stderr,
    "usage:\n"
    "  vbmeta-graft list      <vbmeta-or-partition-img>\n"
    "  vbmeta-graft check     <candidate-part-img> <main-vbmeta-img> <part>\n"
    "  vbmeta-graft graft     --stock <s> --custom <c> --part-size <N> --out <o>\n"
    "  vbmeta-graft list-hash <active-vbmeta-img> <byname-dir>\n");
  return 2;
}

int main(int argc, char **argv)
{
  if (argc < 2) return usage();
  if (strcmp(argv[1], "list") == 0 && argc == 3)
    return cmd_list(argv[2]);
  if (strcmp(argv[1], "check") == 0 && argc == 5)
    return cmd_check(argv[2], argv[3], argv[4]);
  if (strcmp(argv[1], "list-hash") == 0 && argc == 4)
    return cmd_list_hash(argv[2], argv[3]);
  if (strcmp(argv[1], "graft") == 0) {
    const char *stock = NULL, *custom = NULL, *out = NULL;
    uint64_t part_size = 0;
    int i;
    for (i = 2; i + 1 < argc; i += 2) {
      if      (strcmp(argv[i], "--stock")     == 0) stock = argv[i+1];
      else if (strcmp(argv[i], "--custom")    == 0) custom = argv[i+1];
      else if (strcmp(argv[i], "--out")       == 0) out = argv[i+1];
      else if (strcmp(argv[i], "--part-size") == 0)
        part_size = (uint64_t)strtoull(argv[i+1], NULL, 10);
      else return usage();
    }
    if (!stock || !custom || !out || part_size < GBL_AVB_FOOTER_SIZE)
      return usage();
    return cmd_graft(stock, custom, part_size, out);
  }
  return usage();
}
