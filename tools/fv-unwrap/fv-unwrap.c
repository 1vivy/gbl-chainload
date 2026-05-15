/** @file fv-unwrap.c
 *  Extract a PE32+ image from a Qualcomm-style ABL/XBL partition image.
 *
 *  Layout handled:
 *    arm32 ELF wrapper
 *    → FV (Firmware Volume) at some offset within the ELF load segment
 *      → FFS file
 *        → EFI_SECTION_GUID_DEFINED (LZMA, EE4E5898-...)
 *          → decompressed blob (nested FV or raw PE)
 *            → PE32 section or bare MZ/PE
 *
 *  Host-side plain C, links liblzma.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef FVUNWRAP_VENDORED_LZMA
# include "lzmasdk/lzma_compat.h"
#else
# include <lzma.h>
#endif

/* =========================================================================
 * Result type: a heap-allocated PE blob
 * ====================================================================== */

typedef struct {
  uint8_t *data;   /* heap-allocated, caller frees */
  size_t   size;
} PeBlob;

static PeBlob PE_NONE = { NULL, 0 };

/* =========================================================================
 * PE helpers
 * ====================================================================== */

/* Extract PE from arbitrary buffer.  Returns heap copy of the PE or PE_NONE. */
static PeBlob find_and_extract_pe (const uint8_t *buf, size_t size)
{
  for (size_t i = 0; i + 0x80 + 4 < size; ++i) {
    if (buf[i] != 'M' || buf[i+1] != 'Z') continue;
    uint32_t e_lfanew = (uint32_t)buf[i+0x3C]
                      | ((uint32_t)buf[i+0x3D] << 8)
                      | ((uint32_t)buf[i+0x3E] << 16)
                      | ((uint32_t)buf[i+0x3F] << 24);
    size_t peoff = i + e_lfanew;
    if (peoff + 4 >= size) continue;
    if (buf[peoff] != 'P' || buf[peoff+1] != 'E' ||
        buf[peoff+2] != 0  || buf[peoff+3] != 0)   continue;
    /* OptHdr starts at PE+4+20; SizeOfImage at OptHdr+56 */
    size_t opthdr = peoff + 4 + 20;
    if (opthdr + 60 >= size) continue;
    uint32_t sz = (uint32_t)buf[opthdr+56]
               | ((uint32_t)buf[opthdr+57] << 8)
               | ((uint32_t)buf[opthdr+58] << 16)
               | ((uint32_t)buf[opthdr+59] << 24);
    if (sz == 0 || i + sz > size) sz = (uint32_t)(size - i);
    uint8_t *copy = malloc (sz);
    if (!copy) return PE_NONE;
    memcpy (copy, buf + i, sz);
    fprintf (stderr, "  PE32: MZ at +0x%zx SizeOfImage=0x%x\n", i, sz);
    return (PeBlob){ copy, sz };
  }
  return PE_NONE;
}

/* =========================================================================
 * LZMA
 * ====================================================================== */

/* EDK2 LZMA GUID stored LE: EE4E5898-3914-4259-9D6E-DC7BD79403CF */
static const uint8_t kLzmaGuid[16] = {
  0x98,0x58,0x4E,0xEE, 0x14,0x39, 0x59,0x42,
  0x9D,0x6E, 0xDC,0x7B,0xD7,0x94,0x03,0xCF
};

/* Decompress LZMA_ALONE stream.  Returns heap buffer or NULL.
   When FVUNWRAP_VENDORED_LZMA is defined, lzma_compat.h (included above)
   provides this function using the Igor Pavlov SDK instead of liblzma. */
#ifndef FVUNWRAP_VENDORED_LZMA
static uint8_t *lzma_alone_decompress (const uint8_t *in, size_t inSz,
                                       size_t *outSz)
{
  if (inSz < 13) return NULL;
  uint64_t unc64;
  memcpy (&unc64, in + 5, 8);
  size_t alloc = (unc64 == UINT64_MAX || unc64 > 256*1024*1024)
                 ? 256*1024*1024 : (size_t)unc64;
  uint8_t *out = malloc (alloc);
  if (!out) return NULL;

  lzma_stream strm = LZMA_STREAM_INIT;
  if (lzma_alone_decoder (&strm, UINT64_MAX) != LZMA_OK) { free(out); return NULL; }
  strm.next_in   = in;
  strm.avail_in  = inSz;
  strm.next_out  = out;
  strm.avail_out = alloc;
  lzma_ret r = lzma_code (&strm, LZMA_FINISH);
  *outSz = alloc - strm.avail_out;
  lzma_end (&strm);
  if (r != LZMA_STREAM_END && r != LZMA_OK) {
    fprintf (stderr, "  lzma_code: error %d (decoded %zu bytes)\n", (int)r, *outSz);
    /* Don't fail hard — sometimes EDK2 streams end with LZMA_BUF_ERROR */
    if (*outSz == 0) { free(out); return NULL; }
  }
  return out;
}
#endif /* !FVUNWRAP_VENDORED_LZMA */

/* =========================================================================
 * FV / FFS / Section walker  (returns heap PeBlob)
 * ====================================================================== */

static inline uint32_t u24le (const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16); }

/* Forward declaration */
static PeBlob fv_extract_pe (const uint8_t *fv, size_t fvSz, int depth);

#define EFI_SECTION_COMPRESSION   0x01
#define EFI_SECTION_GUID_DEFINED  0x02
#define EFI_SECTION_PE32          0x10
#define EFI_SECTION_FV_IMAGE      0x17

static PeBlob walk_sections (const uint8_t *ffs_body, size_t body_sz, int depth)
{
  size_t off = 0;
  while (off + 4 <= body_sz) {
    if (off & 3) { off = (off+3)&~(size_t)3; continue; }
    uint32_t sz    = u24le (ffs_body + off);
    uint8_t  stype = ffs_body[off + 3];
    if (sz < 4 || off + sz > body_sz) break;

    fprintf (stderr, "%*sSEC type=0x%02x size=0x%x @ body+0x%zx\n",
             depth*2, "", stype, sz, off);

    if (stype == EFI_SECTION_PE32) {
      PeBlob r = find_and_extract_pe (ffs_body + off + 4, sz - 4);
      if (r.data) return r;

    } else if (stype == EFI_SECTION_GUID_DEFINED) {
      /* GUID_DEFINED header after 4-byte section hdr:
         GUID(16) + DataOffset(2) + Attributes(2) */
      if (sz < 4 + 20) { off += sz; continue; }
      const uint8_t *guid     = ffs_body + off + 4;
      uint16_t       data_off = (uint16_t)ffs_body[off+20]
                              | ((uint16_t)ffs_body[off+21]<<8);
      if (memcmp (guid, kLzmaGuid, 16) == 0) {
        fprintf (stderr, "%*sLZMA payload at sec+0x%x\n", depth*2, "", data_off);
        const uint8_t *payload  = ffs_body + off + data_off;
        size_t         pay_sz   = sz - data_off;
        size_t         dec_sz   = 0;
        uint8_t       *dec      = lzma_alone_decompress (payload, pay_sz, &dec_sz);
        if (dec) {
          fprintf (stderr, "%*sDecompressed: %zu bytes\n", depth*2, "", dec_sz);
          /* Try nested FV first, then bare PE scan */
          PeBlob r = fv_extract_pe (dec, dec_sz, depth + 1);
          if (!r.data) r = find_and_extract_pe (dec, dec_sz);
          free (dec);
          if (r.data) return r;
        }
      }

    } else if (stype == EFI_SECTION_COMPRESSION) {
      /* EFI_SECTION_COMPRESSION: 4-byte sec hdr + 4-byte UncompressedLength
         + 1-byte CompressionType.  Type 0=none, 1=EFI (tiano), 2=LZMA custom */
      /* Not handling tiano here; LZMA is covered via GUID_DEFINED above */
      fprintf (stderr, "%*sCOMPRESSION section (tiano?) — skipping\n",
               depth*2, "");

    } else if (stype == EFI_SECTION_FV_IMAGE) {
      PeBlob r = fv_extract_pe (ffs_body + off + 4, sz - 4, depth + 1);
      if (r.data) return r;
    }

    off += sz;
  }
  return PE_NONE;
}

/* Walk an FV: find all FFS files, walk their sections. */
static PeBlob fv_extract_pe (const uint8_t *fv, size_t fvSz, int depth)
{
  if (fvSz < 0x48) return PE_NONE;

  /* Verify _FVH signature at offset +40 */
  if (memcmp (fv + 40, "_FVH", 4) != 0) {
    fprintf (stderr, "%*sFV sig mismatch at depth %d\n", depth*2, "", depth);
    return PE_NONE;
  }
  uint64_t fv_len = 0;
  memcpy (&fv_len, fv + 32, 8);
  uint16_t hdr_len = (uint16_t)fv[48] | ((uint16_t)fv[49]<<8);
  if (fv_len > fvSz) fv_len = fvSz;
  fprintf (stderr, "%*sFV: len=0x%llx hdr=0x%x\n",
           depth*2, "", (unsigned long long)fv_len, hdr_len);

  size_t off = hdr_len;
  while (off + 24 <= fv_len) {
    if (off & 7) { off = (off+7)&~(size_t)7; continue; }
    /* FFS file header: GUID(16)+integrity(2)+type(1)+attrs(1)+size3(3)+state(1) */
    uint32_t fsz = u24le (fv + off + 20);
    uint8_t  ftype = fv[off + 18];
    if (fsz == 0 || fsz == 0xFFFFFF) break;
    if (off + fsz > fv_len) break;
    fprintf (stderr, "%*sFFS type=0x%02x size=0x%x @ fv+0x%zx\n",
             depth*2, "", ftype, fsz, off);

    /* FFS body starts after 24-byte header */
    PeBlob r = walk_sections (fv + off + 24, fsz - 24, depth + 1);
    if (r.data) return r;

    off += fsz;
  }
  return PE_NONE;
}

/* =========================================================================
 * Locate the FV within the raw partition image (may be ELF-wrapped).
 * Scan for _FVH (at fv_start+40).
 * ====================================================================== */

static PeBlob extract_from_partition (const uint8_t *buf, size_t size)
{
  /* Scan for FV headers */
  for (size_t i = 40; i + 8 < size; ++i) {
    if (buf[i]   == '_' && buf[i+1] == 'F' &&
        buf[i+2] == 'V' && buf[i+3] == 'H') {
      size_t fv_start = i - 40;
      fprintf (stderr, "FV candidate at 0x%zx\n", fv_start);
      PeBlob r = fv_extract_pe (buf + fv_start, size - fv_start, 0);
      if (r.data) return r;
    }
  }
  /* Fallback: bare MZ/PE scan */
  fprintf (stderr, "No FV found — trying bare MZ/PE scan\n");
  return find_and_extract_pe (buf, size);
}

/* =========================================================================
 * main
 * ====================================================================== */

int main (int argc, char **argv)
{
  if (argc != 3) {
    fprintf (stderr, "Usage: %s <partition.bin> <output.efi>\n", argv[0]);
    return 2;
  }

  FILE *f = fopen (argv[1], "rb");
  if (!f) { perror (argv[1]); return 1; }
  fseek (f, 0, SEEK_END);
  long raw_sz = ftell (f);
  fseek (f, 0, SEEK_SET);
  if (raw_sz <= 0) { fprintf (stderr, "%s: empty or unreadable\n", argv[1]); return 1; }

  uint8_t *buf = malloc ((size_t)raw_sz);
  if (!buf) { fprintf (stderr, "out of memory\n"); return 1; }
  if (fread (buf, 1, (size_t)raw_sz, f) != (size_t)raw_sz) {
    fprintf (stderr, "%s: read error\n", argv[1]); free(buf); return 1;
  }
  fclose (f);

  PeBlob pe = extract_from_partition (buf, (size_t)raw_sz);
  free (buf);

  if (!pe.data) {
    fprintf (stderr, "%s: no PE32 found\n", argv[1]);
    return 1;
  }

  FILE *o = fopen (argv[2], "wb");
  if (!o) { perror (argv[2]); free(pe.data); return 1; }
  fwrite (pe.data, 1, pe.size, o);
  fclose (o);
  fprintf (stderr, "wrote 0x%zx (%zu) bytes to %s\n", pe.size, pe.size, argv[2]);
  free (pe.data);
  return 0;
}
