/* crates/gblp1/include/gblp1_ffi.h — C ABI for libgblp1.a.
 *
 * Replaces the deleted headers in
 * GblChainloadPkg/Library/GblPayloadLib/Internal/:
 *   - PayloadParse.h   (parser + manifest)
 *   - Sha256.h         (single-shot + streaming SHA-256)
 *   - Crc32.h          (IEEE-802.3 CRC-32)
 *
 * Post-PR2-Task-8: this header is the SOLE C-side source of truth for
 * the GBLP1 container wire constants (magic, version, type IDs,
 * manifest layout). The legacy `tools/shared/gblp1.h` is gone — the
 * host C tools that consumed it are now folded into the `gbl`
 * multicall, and the firmware (`GblPayloadLib`) consumes the
 * definitions from here too.
 *
 * Backed by crates/gblp1 (Rust). Symbols are exported by the
 * libgblp1.a staticlib that cargo builds; each host or firmware
 * consumer links the matching target's staticlib.
 */
#ifndef GBLP1_FFI_H_
#define GBLP1_FFI_H_

#ifdef GBL_HOST_BUILD
# include <stdint.h>
# include <stddef.h>
#else
# include <Uefi.h>
# ifndef GBL_COMPAT_TYPES_DEFINED
#  define GBL_COMPAT_TYPES_DEFINED
   typedef UINT8  uint8_t;
   typedef UINT16 uint16_t;
   typedef UINT32 uint32_t;
   typedef INT32  int32_t;
# endif
# ifndef _SIZE_T
#  define _SIZE_T
   typedef UINTN size_t;
# endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- GBLP1 v1 wire constants (formerly tools/shared/gblp1.h) ---------
 *
 * Layout (LE):
 *   header  ........... 28 bytes (struct gblp1_header below)
 *   entries[] ......... 48 bytes each (struct gblp1_entry below); 16-byte
 *                       aligned start offset right after the header
 *   payloads .......... aligned to 16-byte boundary
 *   footer  ............ 8 bytes ("GBLP1END")
 */
#define GBLP1_MAGIC          "GBLP1\0\0\0"
#define GBLP1_MAGIC_SIZE     8u
#define GBLP1_VERSION        0x0001u
#define GBLP1_HEADER_SIZE    28u
#define GBLP1_FLAGS_LE       0x00000001u
#define GBLP1_FOOTER         "GBLP1END"
#define GBLP1_FOOTER_SIZE    8u
#define GBLP1_TOTAL_SIZE_CAP (16u * 1024u * 1024u)
#define GBLP1_PAYLOAD_ALIGN  16u

#define GBLP1_TYPE_CACHED_ABL    0x0001u
#define GBLP1_TYPE_SOURCE_META   0x0002u
#define GBLP1_TYPE_MODE2_PROFILE 0x0010u  /* mode-2 profile (GM2P) */
#define GBLP1_TYPE_MANIFEST      0x0020u  /* engine capability manifest (GMAN) */

/* Manifest payload (16 bytes; little-endian).
   Layout: magic[4] | schema_version u16 | cap_bits u16 | reserved_pad[8] */
#define GBLP1_MANIFEST_MAGIC                "GMAN"
#define GBLP1_MANIFEST_MAGIC_SIZE           4u
#define GBLP1_MANIFEST_SIZE                 16u
#define GBLP1_MANIFEST_SCHEMA_VERSION       1u
#define GBLP1_MANIFEST_BIT_FAKELOCK_HOOK    0x0001u
#define GBLP1_MANIFEST_BIT_PROFILE_SPOOF    0x0002u
#define GBLP1_MANIFEST_BITS_RESERVED_MASK   0xFFFCu  /* bits 2..15 must be 0 */

#define GBLP1_ENTRY_SIZE     48u

/* On-disk header — must be packed and LE. */
struct gblp1_header {
    uint8_t  magic[8];        /* "GBLP1\0\0\0" */
    uint16_t version;         /* 1 */
    uint16_t header_size;     /* 28 */
    uint32_t flags;           /* bit0 = LE marker */
    uint32_t total_size;      /* entire container */
    uint32_t entry_count;     /* >= 1 */
    uint32_t header_crc32;    /* CRC32 over bytes [0..24) */
};

struct gblp1_entry {
    uint16_t type;
    uint16_t flags;           /* must be 0 in v1 */
    uint32_t payload_offset;  /* absolute, 16-byte aligned */
    uint32_t payload_size;
    uint32_t reserved;        /* must be 0 */
    uint8_t  sha256[32];
};

#ifndef GBLP1_FFI_NO_STATIC_ASSERTS
/* Compile-time size guards. _Static_assert is C11; both EDK2 (clang) and
 * the host toolchains used by tests/host/helpers ship C11-capable
 * compilers. Callers compiling with stricter dialects can `#define
 * GBLP1_FFI_NO_STATIC_ASSERTS` before including. */
_Static_assert(sizeof(struct gblp1_header) == GBLP1_HEADER_SIZE,
               "gblp1_header must be 28 bytes packed");
_Static_assert(sizeof(struct gblp1_entry) == GBLP1_ENTRY_SIZE,
               "gblp1_entry must be 48 bytes packed");
#endif

/* ---- Status enum ------------------------------------------------------
 *
 * Numeric values match the legacy `enum gbl_payload_status` from
 * Internal/PayloadParse.h one-for-one. The Rust shim (`GblPayloadStatus`
 * in crates/gblp1/src/ffi.rs) asserts these discriminants in a unit
 * test. Re-numbering any variant is a wire-ABI break.
 */
enum gbl_payload_status {
    GBL_PAYLOAD_OK                     = 0,
    GBL_PAYLOAD_TOO_SMALL              = 1,
    GBL_PAYLOAD_BAD_MAGIC              = 2,
    GBL_PAYLOAD_BAD_VERSION            = 3,
    GBL_PAYLOAD_BAD_HEADER_SIZE        = 4,
    GBL_PAYLOAD_BAD_FLAGS              = 5,
    GBL_PAYLOAD_BAD_TOTAL_SIZE         = 6,
    GBL_PAYLOAD_BAD_ENTRY_COUNT        = 7,
    GBL_PAYLOAD_HEADER_CRC_MISMATCH    = 8,
    GBL_PAYLOAD_FOOTER_MISMATCH        = 9,
    GBL_PAYLOAD_ENTRY_BAD_TYPE         = 10,
    GBL_PAYLOAD_ENTRY_BAD_FLAGS        = 11,
    GBL_PAYLOAD_ENTRY_BAD_RESERVED     = 12,
    GBL_PAYLOAD_ENTRY_BAD_OFFSET       = 13,
    GBL_PAYLOAD_ENTRY_BAD_SIZE         = 14,
    GBL_PAYLOAD_ENTRY_SHA_MISMATCH     = 15,
    GBL_PAYLOAD_NO_CACHED_ABL          = 16,
    GBL_PAYLOAD_NO_MODE2_PROFILE       = 17,
    /* Reserved; not currently returned. Manifest absence signaled via
     * GBL_PAYLOAD_OK + *out_present == 0. */
    GBL_PAYLOAD_NO_MANIFEST            = 18,
    GBL_PAYLOAD_BAD_MANIFEST_MAGIC     = 19,
    GBL_PAYLOAD_BAD_MANIFEST_SCHEMA    = 20,
    GBL_PAYLOAD_BAD_MANIFEST_RESERVED  = 21,
    GBL_PAYLOAD_BAD_MANIFEST_SIZE      = 22
};

/* ---- Parser API ------------------------------------------------------- */

/* Validates the GBLP1 header + footer layout only (no entry walk). */
enum gbl_payload_status
gbl_payload_validate_header(const uint8_t *bytes, size_t size);

/* Parses + walks the container, locating the unique CACHED_ABL entry.
 * On OK, *out_pe aliases `bytes`; *out_pe_size is its length. */
enum gbl_payload_status
gbl_payload_find_cached_abl(const uint8_t *bytes, size_t size,
                            const uint8_t **out_pe, size_t *out_pe_size);

/* Same as find_cached_abl but for the MODE2_PROFILE entry. */
enum gbl_payload_status
gbl_payload_find_mode2_profile(const uint8_t *bytes, size_t size,
                               const uint8_t **out_profile,
                               size_t *out_size);

/* Scan bytes[0..size) for a GBLP1 container, tolerating stray copies
 * of the 8-byte magic. Returns the FIRST occurrence that fully
 * validates, the last non-OK status if the magic was seen but none
 * validated, or GBL_PAYLOAD_BAD_MAGIC if the magic was never found. */
enum gbl_payload_status
gbl_payload_scan_cached_abl(const uint8_t *bytes, size_t size,
                            const uint8_t **out_pe, size_t *out_pe_size);

/* Engine capability manifest. cap_bits is the raw bit field from the
 * wire, validated against GBLP1_MANIFEST_BITS_RESERVED_MASK but
 * otherwise passed through — callers compare against the
 * GBLP1_MANIFEST_BIT_* constants in tools/shared/gblp1.h. */
struct gbl_manifest { uint16_t cap_bits; };

/* Locate + validate the unique MANIFEST entry. On OK with
 * *out_present == 1: *out is filled. On *out_present == 0: no manifest
 * entry exists in the container (NOT an error; caller treats as
 * all-zero capabilities). On any other return: parse or validation
 * failed; *out is undefined. */
enum gbl_payload_status
gbl_payload_find_manifest(const uint8_t *bytes, size_t size,
                          struct gbl_manifest *out, int *out_present);

/* ---- SHA-256 ----------------------------------------------------------
 *
 * The streaming context is opaque. Callers allocate it on the stack
 * (or wherever) at the C-side size; the Rust crate owns the layout
 * inside. Size + alignment are pinned here and the Rust shim has a
 * compile-time assert that the inner `sha2::Sha256` fits.
 *
 * Single-shot: gbl_sha256(buf, len, out32).
 * Streaming:   gbl_sha256_init / _update / _final on a gbl_sha256_ctx.
 */
#define GBL_SHA256_CTX_SIZE 256u

/* 8-byte alignment for the opaque blob — the Rust crate places a
 * sha2::Sha256 (which needs 8-byte alignment) at offset 8 of this
 * buffer, behind an 8-byte tag.
 *
 * We use `void *` as the alignment proxy (always at least 8-byte on
 * AArch64/x86_64) to avoid pulling in <stdalign.h> or typedef'ing
 * uint64_t in a way that clashes with stdint.h (the GBLP1 wire
 * constants header includes stdint.h unconditionally; some
 * stdint.h backends typedef uint64_t as `unsigned long` while EDK2's
 * UINT64 is `unsigned long long`, and the resulting clash kills the
 * EDK2 build).
 */
typedef struct {
    /* First slot is the type-erased alignment proxy; the rest is byte
     * storage. Total size is GBL_SHA256_CTX_SIZE — treat as fully
     * opaque. */
    void    *_align;
    uint8_t  bytes[GBL_SHA256_CTX_SIZE - sizeof(void *)];
} gbl_sha256_ctx;

void gbl_sha256(const uint8_t *buf, size_t len, uint8_t out[32]);
void gbl_sha256_init(gbl_sha256_ctx *ctx);
void gbl_sha256_update(gbl_sha256_ctx *ctx, const uint8_t *data, size_t len);
void gbl_sha256_final(gbl_sha256_ctx *ctx, uint8_t out[32]);

/* ---- CRC-32 ----------------------------------------------------------- */

uint32_t gbl_crc32(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* GBLP1_FFI_H_ */
