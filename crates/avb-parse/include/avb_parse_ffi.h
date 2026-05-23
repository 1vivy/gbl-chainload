/* crates/avb-parse/include/avb_parse_ffi.h — C ABI for libavb_parse.a.
 *
 * Replaces:
 *   - GblChainloadPkg/Library/AvbParseLib/Internal/AvbBigEndian.h
 *   - the function declarations in GblChainloadPkg/Include/Library/
 *     AvbParseLib.h (the public header now just re-includes this).
 *
 * Backed by crates/avb-parse (Rust). Symbols are exported by the
 * libavb_parse.a staticlib that cargo builds; each host or firmware
 * consumer links the matching target's staticlib.
 *
 * Wire-ABI commitment: every struct + enum below matches the deleted
 * C library's byte layout / discriminants exactly. The Rust shim
 * (`crates/avb-parse/src/ffi.rs`) asserts these in unit tests; the
 * 074 + 090 + 091 host tests' goldens lock the runtime behavior.
 */
#ifndef AVB_PARSE_FFI_H_
#define AVB_PARSE_FFI_H_

/* Type shims — pulled in by the host-build C tools that don't include
 * <Uefi.h>. The firmware build path takes the `else` branch and pulls
 * UEFI's real types. */
#if defined(__HOST_BUILD__) || defined(GBL_HOST_BUILD)
# include <stdint.h>
# include <stddef.h>
# ifndef UINT8
   typedef uint8_t  UINT8;
# endif
# ifndef UINT32
   typedef uint32_t UINT32;
# endif
# ifndef UINT64
   typedef uint64_t UINT64;
# endif
# ifndef CHAR8
   typedef char     CHAR8;
# endif
# ifndef EFI_STATUS
   typedef long     EFI_STATUS;
# endif
# ifndef EFIAPI
#  define EFIAPI
# endif
# ifndef IN
#  define IN
# endif
# ifndef OUT
#  define OUT
# endif
# ifndef STATIC
#  define STATIC static
# endif
# ifndef CONST
#  define CONST const
# endif
# ifndef OPTIONAL
#  define OPTIONAL
# endif
# ifndef EFI_SUCCESS
   /* Match UEFI's high-bit-set encoding of error codes — the firmware
    * build picks these up from <Uefi.h>. Host EFI_STATUS is a `long`
    * (signed); the high bit gives us the same "negative = error"
    * predicate `EFI_ERROR(s) := s != 0` callers already use. */
#  define EFI_SUCCESS              ((EFI_STATUS)0)
# endif
# ifndef EFI_INVALID_PARAMETER
#  define EFI_INVALID_PARAMETER    ((EFI_STATUS)0x8000000000000002LL)
# endif
# ifndef EFI_NOT_FOUND
#  define EFI_NOT_FOUND            ((EFI_STATUS)0x800000000000000ELL)
# endif
# ifndef EFI_END_OF_MEDIA
#  define EFI_END_OF_MEDIA         ((EFI_STATUS)0x800000000000001CLL)
# endif
# ifndef EFI_ERROR
#  define EFI_ERROR(s)             ((s) != 0)
# endif
#else
# include <Uefi.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- magic + sizes ----------------------------------------------------
 * Mirrors the GBL_AVB_* macros from the deleted AvbParseLib.h. */
#define GBL_AVB_FOOTER_MAGIC        "AVBf"
#define GBL_AVB_VBMETA_MAGIC        "AVB0"
#define GBL_AVB_FOOTER_SIZE         64
#define GBL_AVB_VBMETA_HEADER_SIZE  256

/* ---- GBL_AVB_FOOTER ----------------------------------------------------
 * Decoded trailing 64-byte AvbFooter. Field order + sizes match the
 * deleted C struct exactly; the Rust shim asserts size_of == 32. */
typedef struct {
  UINT32  FooterMajorVersion;
  UINT32  FooterMinorVersion;
  UINT64  OriginalImageSize;
  UINT64  VbmetaOffset;
  UINT64  VbmetaSize;
} GBL_AVB_FOOTER;

/* ---- GBL_AVB_VBMETA_HEADER --------------------------------------------
 * Decoded 256-byte AvbVBMetaImageHeader. The Rust shim asserts
 * size_of == 160 (the layout after natural alignment, NOT the on-disk
 * 256-byte size — the on-disk image has reserved padding at the tail
 * that the parser drops). */
typedef struct {
  UINT32  AvbMajorVersion;
  UINT32  AvbMinorVersion;
  UINT64  AuthenticationDataBlockSize;
  UINT64  AuxiliaryDataBlockSize;
  UINT32  AlgorithmType;
  UINT64  HashOffset;
  UINT64  HashSize;
  UINT64  SignatureOffset;
  UINT64  SignatureSize;
  UINT64  PublicKeyOffset;
  UINT64  PublicKeySize;
  UINT64  PublicKeyMetadataOffset;
  UINT64  PublicKeyMetadataSize;
  UINT64  DescriptorsOffset;
  UINT64  DescriptorsSize;
  UINT64  RollbackIndex;
  UINT32  Flags;
  UINT32  RollbackIndexLocation;
  CHAR8   ReleaseString[48];
} GBL_AVB_VBMETA_HEADER;

/* ---- GBL_AVB_DESCRIPTOR_TAG --------------------------------------- */
typedef enum {
  GblAvbDescPropertyTag        = 0,
  GblAvbDescHashtreeTag        = 1,
  GblAvbDescHashTag            = 2,
  GblAvbDescKernelCmdlineTag   = 3,
  GblAvbDescChainPartitionTag  = 4,
} GBL_AVB_DESCRIPTOR_TAG;

/* ---- GBL_AVB_CHAIN_VERDICT ----------------------------------------
 * Key-identity check buckets — see `AvbParse_ChainVerdict` below. */
typedef enum {
  GblAvbChainOk          = 0,  /* embedded vbmeta + key matches chain descriptor */
  GblAvbChainKeyMismatch = 1,  /* vbmeta parsed but key != chain descriptor */
  GblAvbChainNoVbmeta    = 2,  /* no footer / unparseable / malformed */
} GBL_AVB_CHAIN_VERDICT;

/* ---- AVB big-endian helpers ----------------------------------------
 * `static inline` so callers that include this header pick them up
 * directly without needing an extra translation unit. The deleted
 * AvbBigEndian.h provided the same two helpers in `STATIC` form;
 * vbmeta-graft.c (host) + tools/mode2-profile/mode2-profile.c (host)
 * still call them directly. */
static inline UINT32 AvbReadU32Be (CONST UINT8 *Buf) {
  return ((UINT32)Buf[0] << 24) | ((UINT32)Buf[1] << 16)
       | ((UINT32)Buf[2] << 8)  |  (UINT32)Buf[3];
}

static inline UINT64 AvbReadU64Be (CONST UINT8 *Buf) {
  return ((UINT64)Buf[0] << 56) | ((UINT64)Buf[1] << 48)
       | ((UINT64)Buf[2] << 40) | ((UINT64)Buf[3] << 32)
       | ((UINT64)Buf[4] << 24) | ((UINT64)Buf[5] << 16)
       | ((UINT64)Buf[6] << 8)  |  (UINT64)Buf[7];
}

/* ---- C ABI entry points -------------------------------------------
 * Signatures are reproduced verbatim from the deleted AvbParseLib.h.
 * Symbol names are stable so the firmware (`FastbootCmds.c`) and host
 * tools (`vbmeta-graft`, `mode2-profile`, `tests/avb`) link unchanged. */

EFI_STATUS EFIAPI AvbParse_Footer (
  IN CONST UINT8 *Partition,
  IN UINT64 PartitionSize,
  OUT GBL_AVB_FOOTER *FooterOut);

EFI_STATUS EFIAPI AvbParse_VbmetaHeader (
  IN CONST UINT8 *Vbmeta,
  IN UINT64 VbmetaSize,
  OUT GBL_AVB_VBMETA_HEADER *HeaderOut);

EFI_STATUS EFIAPI AvbParse_NextDescriptor (
  IN CONST UINT8 *AuxBlock,
  IN UINT64 AuxSize,
  IN OUT UINT64 *Cursor,
  OUT GBL_AVB_DESCRIPTOR_TAG *TagOut,
  OUT CONST UINT8 **DescriptorOut,
  OUT UINT64 *DescriptorLenOut);

EFI_STATUS EFIAPI AvbParse_HashDescriptor (
  IN CONST UINT8   *Descriptor,
  IN UINT64         DescriptorLen,
  OUT CONST UINT8 **PartitionNameOut,
  OUT UINT32       *PartitionNameLenOut,
  OUT CONST UINT8 **DigestOut,
  OUT UINT32       *DigestLenOut,
  OUT CONST UINT8 **SaltOut OPTIONAL,
  OUT UINT32       *SaltLenOut OPTIONAL,
  OUT UINT64       *ImageSizeOut OPTIONAL);

EFI_STATUS EFIAPI AvbParse_ChainPartitionDescriptor (
  IN CONST UINT8 *Descriptor,
  IN UINT64 DescriptorLen,
  OUT CONST UINT8 **PartitionNameOut,
  OUT UINT32 *PartitionNameLenOut,
  OUT CONST UINT8 **PublicKeyOut,
  OUT UINT32 *PublicKeyLenOut);

EFI_STATUS EFIAPI AvbParse_FooterFromTail (
  IN CONST UINT8 *Tail,
  IN UINT64 TailLen,
  IN UINT64 PartitionSize,
  OUT GBL_AVB_FOOTER *FooterOut);

EFI_STATUS EFIAPI AvbParse_ChainVerdict (
  IN CONST UINT8 *Vbmeta,
  IN UINT64 VbmetaSize,
  IN CONST UINT8 *ChainPk OPTIONAL,
  IN UINT32 ChainPkLen,
  OUT GBL_AVB_CHAIN_VERDICT *VerdictOut);

#ifdef __cplusplus
}
#endif

#endif /* AVB_PARSE_FFI_H_ */
