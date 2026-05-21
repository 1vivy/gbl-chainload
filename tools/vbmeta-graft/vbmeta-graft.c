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
#include <inttypes.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
# include <sys/ioctl.h>
# include <linux/fs.h>   /* BLKGETSIZE64 */
#endif

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

/* bd_open_size: open a path (regular file or block device) and return its
 * total byte size.  For block devices, ioctl(BLKGETSIZE64) is used on Linux;
 * on other platforms lseek(SEEK_END) is used (works for regular files and
 * most block device implementations).  Returns -1 on error. */
static int64_t bd_open_size(const char *path, int *fd_out)
{
  int fd = open(path, O_RDONLY);
  if (fd < 0) { fprintf(stderr, "vbmeta-graft: %s: cannot open\n", path); return -1; }
  struct stat st;
  if (fstat(fd, &st) != 0) {
    fprintf(stderr, "vbmeta-graft: %s: fstat error\n", path);
    close(fd); return -1;
  }
  int64_t size = -1;
#if defined(__linux__) && defined(BLKGETSIZE64)
  if (S_ISBLK(st.st_mode)) {
    uint64_t blksz = 0;
    if (ioctl(fd, BLKGETSIZE64, &blksz) == 0)
      size = (int64_t)blksz;
    else {
      fprintf(stderr, "vbmeta-graft: %s: BLKGETSIZE64 failed\n", path);
      close(fd); return -1;
    }
  }
#endif
  if (size < 0) {
    /* Regular file or non-Linux block device: use lseek. */
    off_t end = lseek(fd, 0, SEEK_END);
    if (end < 0) {
      fprintf(stderr, "vbmeta-graft: %s: lseek error\n", path);
      close(fd); return -1;
    }
    size = (int64_t)end;
    if (lseek(fd, 0, SEEK_SET) < 0) {
      fprintf(stderr, "vbmeta-graft: %s: lseek error\n", path);
      close(fd); return -1;
    }
  }
  *fd_out = fd;
  return size;
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
    AvbParse_HashDescriptor(desc, desc_len, &name, &name_len, &digest, &digest_len,
                            NULL, NULL, NULL);
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

/* (find_avb0 / probe_graft tail-scan path retired 2026-05-20 — see
 * probe_partition_for_graft below for the AvbFooter-walking replacement.) */

/* Probe a partition file for an OEM-signed-vbmeta graft. Walks the way
 * libavb does: read the AvbFooter from the last 64 bytes, follow it to
 * the vbmeta blob at footer.VbmetaOffset, parse the header, then check
 * the embedded public key matches the parent's chain descriptor.
 *
 * Returns:
 *   GRAFT_OK            — footer present, vbmeta parseable, key matches
 *                         (AOSP first-stage init libavb will accept it).
 *   GRAFT_KEY_MISMATCH  — footer present, vbmeta parseable, but the
 *                         embedded key does not match the chain
 *                         descriptor's key. AVB sig-verify would fail.
 *   GRAFT_NO_VBMETA     — no AvbFooter at end-of-partition (= libavb's
 *                         `ok_not_signed` path, the mode-1 boot-blocker
 *                         per docs/project/vbmeta-graft-vs-construct.md §2b).
 *
 * NOTE — this is a key-identity check, NOT a signature verification.
 * Full sig-verify would need the OEM private key (impossible by
 * construction) or a libavb-equivalent RSA path. The threat model is
 * operator self-diagnosis of an install, not cryptographic attestation;
 * the chain-descriptor pubkey match is what `init` will check anyway
 * before it trusts the embedded signature, so a key mismatch here is
 * already definitive evidence init would reject it.
 *
 * Why this replaces the old tail-window AVB0 scan: a stock-recovery
 * graft sits at round_up(custom_content_size, 4K) — for a small custom
 * recovery in a 100 MiB partition, that offset is ~37 MiB, well outside
 * any tail window. The AvbFooter at partition_end-64 always points at
 * the real vbmeta location, no matter how far back it is, so walking
 * the footer is both correct and cheap (one 64-byte read + one
 * vbmeta-sized read, regardless of partition size). See
 * docs/project/vbmeta-graft-vs-construct.md §1 / §2 for the byte layout
 * and the two-layer verify flow this is mimicking. */
enum graft_probe_result {
  GRAFT_OK = 0,
  GRAFT_KEY_MISMATCH = 1,
  GRAFT_NO_VBMETA = 2,
};

static enum graft_probe_result
probe_partition_for_graft(const char *path,
                          const uint8_t *chain_pk, uint32_t chain_pk_len)
{
  enum graft_probe_result result = GRAFT_NO_VBMETA;
  int fd = -1;
  int64_t part_sz = bd_open_size(path, &fd);
  if (part_sz <= 0 || fd < 0) {
    if (fd >= 0) close(fd);
    return GRAFT_NO_VBMETA;
  }
  if ((uint64_t)part_sz < GBL_AVB_FOOTER_SIZE) { close(fd); return GRAFT_NO_VBMETA; }

  /* 1. Read the last 64 bytes and decode the AvbFooter.
   *
   * NB: we deliberately don't call AvbParse_Footer here. That helper
   * assumes its (Partition, PartitionSize) pair describes the *whole*
   * partition (it dereferences `Partition + PartitionSize - 64` and
   * also bounds-checks `VbmetaOffset + VbmetaSize <= PartitionSize`).
   * Slurping a multi-MiB block device just to satisfy that contract is
   * wasteful when all we need is 32 bytes of trailer fields. Hand-roll
   * the same 4-magic + 5-field decode against the 64-byte buffer
   * directly, then bounds-check against the real `part_sz`. */
  uint8_t footer_buf[GBL_AVB_FOOTER_SIZE];
  off_t footer_off = (off_t)((uint64_t)part_sz - GBL_AVB_FOOTER_SIZE);
  if (lseek(fd, footer_off, SEEK_SET) != footer_off) { close(fd); return GRAFT_NO_VBMETA; }
  if (read(fd, footer_buf, GBL_AVB_FOOTER_SIZE) != (ssize_t)GBL_AVB_FOOTER_SIZE) {
    close(fd); return GRAFT_NO_VBMETA;
  }
  if (memcmp(footer_buf, GBL_AVB_FOOTER_MAGIC, 4) != 0) {
    close(fd); return GRAFT_NO_VBMETA;
  }
  GBL_AVB_FOOTER footer;
  footer.FooterMajorVersion = AvbReadU32Be(footer_buf + 4);
  footer.FooterMinorVersion = AvbReadU32Be(footer_buf + 8);
  footer.OriginalImageSize  = AvbReadU64Be(footer_buf + 12);
  footer.VbmetaOffset       = AvbReadU64Be(footer_buf + 20);
  footer.VbmetaSize         = AvbReadU64Be(footer_buf + 28);
  if (footer.VbmetaSize == 0 ||
      footer.VbmetaOffset >= (uint64_t)part_sz ||
      footer.VbmetaSize > (uint64_t)part_sz - footer.VbmetaOffset) {
    close(fd); return GRAFT_NO_VBMETA;
  }

  /* 2. Read the vbmeta blob the footer points to. */
  uint8_t *vb = malloc((size_t)footer.VbmetaSize);
  if (!vb) { close(fd); return GRAFT_NO_VBMETA; }
  if (lseek(fd, (off_t)footer.VbmetaOffset, SEEK_SET) != (off_t)footer.VbmetaOffset) {
    free(vb); close(fd); return GRAFT_NO_VBMETA;
  }
  if (read(fd, vb, (size_t)footer.VbmetaSize) != (ssize_t)footer.VbmetaSize) {
    free(vb); close(fd); return GRAFT_NO_VBMETA;
  }
  close(fd);

  /* 3. Parse the vbmeta header. If this fails, the bytes the footer
   *    points to are not a vbmeta — init would treat the partition as
   *    `ok_not_signed`. */
  GBL_AVB_VBMETA_HEADER vh;
  if (AvbParse_VbmetaHeader(vb, footer.VbmetaSize, &vh) != EFI_SUCCESS) {
    free(vb); return GRAFT_NO_VBMETA;
  }

  /* 4. Compare embedded pubkey to the chain descriptor's pubkey. If
   *    chain_pk is unavailable (caller passed NULL/0), any parseable
   *    vbmeta is a hit — useful for plain `vbmeta list` style usage. */
  uint64_t aux_len;
  const uint8_t *aux = aux_block(vb, &vh, &aux_len);
  if (vh.PublicKeyOffset > aux_len ||
      vh.PublicKeySize   > aux_len - vh.PublicKeyOffset) {
    free(vb); return GRAFT_NO_VBMETA;        /* malformed → treat as no vbmeta */
  }
  const uint8_t *pk = aux + vh.PublicKeyOffset;
  uint32_t       pk_len = (uint32_t)vh.PublicKeySize;
  if (chain_pk && chain_pk_len > 0) {
    if (pk_len == chain_pk_len && memcmp(pk, chain_pk, pk_len) == 0) {
      result = GRAFT_OK;
    } else {
      result = GRAFT_KEY_MISMATCH;
    }
  } else {
    result = GRAFT_OK;
  }
  free(vb);
  return result;
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
    const uint8_t *name   = NULL;
    uint32_t      name_len = 0;
    const uint8_t *digest  = NULL;
    uint32_t      digest_len = 0;
    const uint8_t *salt    = NULL;
    uint32_t      salt_len = 0;
    uint64_t      image_size = 0;
    if (AvbParse_HashDescriptor(desc, desc_len, &name, &name_len,
                                &digest, &digest_len,
                                &salt, &salt_len, &image_size) != EFI_SUCCESS)
      return;

    /* Build partition path */
    char path[4096];
    snprintf(path, sizeof(path), "%s/%.*s_%s",
             ctx->byname_dir, (int)name_len, (const char *)name, ctx->slot);

    /* Compute: SHA-256(salt || partition_bytes[0..image_size))
     * Uses streaming SHA-256 so large (>128 MiB) partitions like system/vendor
     * are handled correctly.  Opens the path with open(O_RDONLY) so block
     * devices (on-device /dev/block/by-name/...) are supported alongside
     * regular files used in host-side tests. */
    const char *digest_status = "missing";
    const char *verdict = "mismatch";
    int part_fd = -1;
    int64_t part_sz = bd_open_size(path, &part_fd);
    if (part_sz > 0) {
      uint64_t read_size = image_size;
      if (read_size > (uint64_t)part_sz)
        read_size = (uint64_t)part_sz;

      /* Streaming SHA-256(salt || content). */
      gbl_sha256_ctx sha_ctx;
      gbl_sha256_init(&sha_ctx);
      if (salt_len > 0)
        gbl_sha256_update(&sha_ctx, salt, salt_len);

      uint8_t chunk_buf[1 << 20]; /* 1 MiB read buffer */
      uint64_t remaining = read_size;
      int read_ok = 1;
      while (remaining > 0) {
        size_t want = (remaining > sizeof(chunk_buf))
                      ? sizeof(chunk_buf) : (size_t)remaining;
        ssize_t n = read(part_fd, chunk_buf, want);
        if (n <= 0) { read_ok = 0; break; }
        gbl_sha256_update(&sha_ctx, chunk_buf, (size_t)n);
        remaining -= (size_t)n;
      }

      if (read_ok) {
        uint8_t got[32];
        gbl_sha256_final(&sha_ctx, got);
        if (digest_len == 32 && memcmp(got, digest, 32) == 0) {
          digest_status = "ok";
          verdict = "match";
        } else {
          digest_status = "mismatch";
          verdict = "mismatch";
        }
      }
      close(part_fd);
    }

    printf("partition=%.*s type=hash declared=%" PRIu64 " digest=%s graft=n/a verdict=%s\n",
           (int)name_len, (const char *)name,
           image_size,
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

    /* Walk the partition's AvbFooter → embedded vbmeta. The buckets:
     *   GRAFT_OK            → init's libavb will accept it; verdict=match
     *   GRAFT_KEY_MISMATCH  → vbmeta exists but signed by a non-OEM key;
     *                         init's sig-verify would fail → graft needed
     *   GRAFT_NO_VBMETA     → init returns `ok_not_signed`; only mode-2
     *                         (orange-state) tolerates it. */
    const char *graft_status = "missing";
    const char *verdict      = "mismatch";
    switch (probe_partition_for_graft(path, chain_pk, chain_pk_len)) {
      case GRAFT_OK:
        graft_status = "ok";
        verdict      = "match";
        break;
      case GRAFT_KEY_MISMATCH:
        graft_status = "key_mismatch";
        verdict      = "mismatch";
        break;
      case GRAFT_NO_VBMETA:
        graft_status = "no_vbmeta";
        verdict      = "mismatch";
        break;
    }

    printf("partition=%.*s type=chain declared=- digest=n/a graft=%s verdict=%s\n",
           (int)name_len, (const char *)name,
           graft_status, verdict);
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
    "vbmeta-graft %s\n"
    "usage:\n"
    "  vbmeta-graft list      <vbmeta-or-partition-img>\n"
    "  vbmeta-graft check     <candidate-part-img> <main-vbmeta-img> <part>\n"
    "  vbmeta-graft graft     --stock <s> --custom <c> --part-size <N> --out <o>\n"
    "  vbmeta-graft list-hash <active-vbmeta-img> <byname-dir>\n",
    GBL_TOOL_VERSION);
  return 2;
}

int main(int argc, char **argv)
{
  if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
    printf("vbmeta-graft %s\n", GBL_TOOL_VERSION);
    return 0;
  }
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
