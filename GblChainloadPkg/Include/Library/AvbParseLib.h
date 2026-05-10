/** @file AvbParseLib.h — minimal AVB structure parser. **/
#ifndef AVB_PARSE_LIB_H_
#define AVB_PARSE_LIB_H_

#ifndef __HOST_BUILD__
#include <Uefi.h>
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

EFI_STATUS EFIAPI AvbParse_Footer (IN CONST UINT8 *Partition, IN UINT64 PartitionSize, OUT GBL_AVB_FOOTER *FooterOut);
EFI_STATUS EFIAPI AvbParse_VbmetaHeader (IN CONST UINT8 *Vbmeta, IN UINT64 VbmetaSize, OUT GBL_AVB_VBMETA_HEADER *HeaderOut);
EFI_STATUS EFIAPI AvbParse_NextDescriptor (IN CONST UINT8 *AuxBlock, IN UINT64 AuxSize, IN OUT UINT64 *Cursor, OUT GBL_AVB_DESCRIPTOR_TAG *TagOut, OUT CONST UINT8 **DescriptorOut, OUT UINT64 *DescriptorLenOut);
EFI_STATUS EFIAPI AvbParse_HashDescriptor (IN CONST UINT8 *Descriptor, IN UINT64 DescriptorLen, OUT CONST UINT8 **PartitionNameOut, OUT UINT32 *PartitionNameLenOut, OUT CONST UINT8 **DigestOut, OUT UINT32 *DigestLenOut);
EFI_STATUS EFIAPI AvbParse_ChainPartitionDescriptor (IN CONST UINT8 *Descriptor, IN UINT64 DescriptorLen, OUT CONST UINT8 **PartitionNameOut, OUT UINT32 *PartitionNameLenOut, OUT CONST UINT8 **PublicKeyOut, OUT UINT32 *PublicKeyLenOut);

#endif
