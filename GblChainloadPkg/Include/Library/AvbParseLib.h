/** @file AvbParseLib.h — minimal AVB structure parser. **/
#ifndef AVB_PARSE_LIB_H_
#define AVB_PARSE_LIB_H_

#ifndef __HOST_BUILD__
#include <Uefi.h>
#endif

/* Self-contained shim: callers that supply their own UEFI type stand-ins
   (e.g., the host test harness) shouldn't have to remember to define
   OPTIONAL separately. Uefi.h already defines it; the guard prevents
   redefinition there. */
#ifndef OPTIONAL
#define OPTIONAL
#endif

#define GBL_AVB_FOOTER_MAGIC        "AVBf"
#define GBL_AVB_VBMETA_MAGIC        "AVB0"
#define GBL_AVB_FOOTER_SIZE         64
#define GBL_AVB_VBMETA_HEADER_SIZE  256

typedef struct {
  UINT32  FooterMajorVersion;
  UINT32  FooterMinorVersion;
  UINT64  OriginalImageSize;
  UINT64  VbmetaOffset;
  UINT64  VbmetaSize;
} GBL_AVB_FOOTER;

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

typedef enum {
  GblAvbDescPropertyTag        = 0,
  GblAvbDescHashtreeTag        = 1,
  GblAvbDescHashTag            = 2,
  GblAvbDescKernelCmdlineTag   = 3,
  GblAvbDescChainPartitionTag  = 4,
} GBL_AVB_DESCRIPTOR_TAG;

/* Verdict from probing a chained partition's embedded vbmeta against the
   chain descriptor's public key — a key-identity check (NOT a signature
   verification), matching what AOSP first-stage init's libavb checks before
   it trusts an embedded signature. Mirrors vbmeta-graft's graft_probe_result. */
typedef enum {
  GblAvbChainOk          = 0,  /* embedded vbmeta present, pubkey matches chain descriptor */
  GblAvbChainKeyMismatch = 1,  /* embedded vbmeta present, pubkey != chain descriptor pubkey */
  GblAvbChainNoVbmeta    = 2,  /* no footer / unparseable vbmeta / malformed → init's ok_not_signed */
} GBL_AVB_CHAIN_VERDICT;

EFI_STATUS EFIAPI AvbParse_Footer (IN CONST UINT8 *Partition, IN UINT64 PartitionSize, OUT GBL_AVB_FOOTER *FooterOut);
EFI_STATUS EFIAPI AvbParse_VbmetaHeader (IN CONST UINT8 *Vbmeta, IN UINT64 VbmetaSize, OUT GBL_AVB_VBMETA_HEADER *HeaderOut);
EFI_STATUS EFIAPI AvbParse_NextDescriptor (IN CONST UINT8 *AuxBlock, IN UINT64 AuxSize, IN OUT UINT64 *Cursor, OUT GBL_AVB_DESCRIPTOR_TAG *TagOut, OUT CONST UINT8 **DescriptorOut, OUT UINT64 *DescriptorLenOut);
EFI_STATUS EFIAPI AvbParse_HashDescriptor (
  IN CONST UINT8   *Descriptor, IN UINT64 DescriptorLen,
  OUT CONST UINT8 **PartitionNameOut, OUT UINT32 *PartitionNameLenOut,
  OUT CONST UINT8 **DigestOut, OUT UINT32 *DigestLenOut,
  OUT CONST UINT8 **SaltOut OPTIONAL, OUT UINT32 *SaltLenOut OPTIONAL,
  OUT UINT64       *ImageSizeOut OPTIONAL);
EFI_STATUS EFIAPI AvbParse_ChainPartitionDescriptor (IN CONST UINT8 *Descriptor, IN UINT64 DescriptorLen, OUT CONST UINT8 **PartitionNameOut, OUT UINT32 *PartitionNameLenOut, OUT CONST UINT8 **PublicKeyOut, OUT UINT32 *PublicKeyLenOut);

/* Decode the trailing 64-byte AvbFooter from a partition tail window without
   holding the whole (multi-MiB) partition in memory. Tail must contain at
   least the final GBL_AVB_FOOTER_SIZE bytes of the partition; the footer is
   read from Tail + TailLen - 64. VbmetaOffset/VbmetaSize are bounds-checked
   against the real PartitionSize, NOT against TailLen (the vbmeta blob the
   footer points at usually lies outside the tail window). Returns
   EFI_NOT_FOUND when the "AVBf" magic is absent. */
EFI_STATUS EFIAPI AvbParse_FooterFromTail (
  IN CONST UINT8 *Tail, IN UINT64 TailLen, IN UINT64 PartitionSize,
  OUT GBL_AVB_FOOTER *FooterOut);

/* Classify a chained partition's embedded vbmeta against the chain
   descriptor's public key (key-identity check, no crypto). Vbmeta points at
   the embedded vbmeta blob (i.e. partition bytes at footer.VbmetaOffset).
   When ChainPk is NULL or ChainPkLen is 0, any parseable vbmeta yields
   GblAvbChainOk. *VerdictOut is always set on EFI_SUCCESS; an unparseable or
   malformed blob yields GblAvbChainNoVbmeta (still EFI_SUCCESS). */
EFI_STATUS EFIAPI AvbParse_ChainVerdict (
  IN CONST UINT8 *Vbmeta, IN UINT64 VbmetaSize,
  IN CONST UINT8 *ChainPk OPTIONAL, IN UINT32 ChainPkLen,
  OUT GBL_AVB_CHAIN_VERDICT *VerdictOut);

#endif
