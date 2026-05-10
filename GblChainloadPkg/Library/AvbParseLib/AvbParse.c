/** @file AvbParse.c — AVB structure parser. **/
#include "Internal/AvbBigEndian.h"

#include "../../Include/Library/AvbParseLib.h"

EFI_STATUS EFIAPI
AvbParse_Footer (IN CONST UINT8 *Partition, IN UINT64 PartitionSize, OUT GBL_AVB_FOOTER *FooterOut) {
  CONST UINT8 *Footer;
  if (Partition == NULL || FooterOut == NULL)        return EFI_INVALID_PARAMETER;
  if (PartitionSize < GBL_AVB_FOOTER_SIZE)           return EFI_INVALID_PARAMETER;
  Footer = Partition + PartitionSize - GBL_AVB_FOOTER_SIZE;
  if (Footer[0] != 'A' || Footer[1] != 'V'
      || Footer[2] != 'B' || Footer[3] != 'f') {
    return EFI_NOT_FOUND;
  }
  FooterOut->FooterMajorVersion  = AvbReadU32Be (Footer + 4);
  FooterOut->FooterMinorVersion  = AvbReadU32Be (Footer + 8);
  FooterOut->OriginalImageSize   = AvbReadU64Be (Footer + 12);
  FooterOut->VbmetaOffset        = AvbReadU64Be (Footer + 20);
  FooterOut->VbmetaSize          = AvbReadU64Be (Footer + 28);
  if (FooterOut->VbmetaOffset + FooterOut->VbmetaSize > PartitionSize) return EFI_INVALID_PARAMETER;
  if (FooterOut->OriginalImageSize > PartitionSize) return EFI_INVALID_PARAMETER;
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
AvbParse_VbmetaHeader (IN CONST UINT8 *Vbmeta, IN UINT64 VbmetaSize, OUT GBL_AVB_VBMETA_HEADER *HeaderOut)
{
  if (Vbmeta == NULL || HeaderOut == NULL)         return EFI_INVALID_PARAMETER;
  if (VbmetaSize < GBL_AVB_VBMETA_HEADER_SIZE)     return EFI_INVALID_PARAMETER;

  if (Vbmeta[0] != 'A' || Vbmeta[1] != 'V'
      || Vbmeta[2] != 'B' || Vbmeta[3] != '0') {
    return EFI_NOT_FOUND;
  }

  HeaderOut->AvbMajorVersion              = AvbReadU32Be (Vbmeta + 4);
  HeaderOut->AvbMinorVersion              = AvbReadU32Be (Vbmeta + 8);
  HeaderOut->AuthenticationDataBlockSize  = AvbReadU64Be (Vbmeta + 12);
  HeaderOut->AuxiliaryDataBlockSize       = AvbReadU64Be (Vbmeta + 20);
  HeaderOut->AlgorithmType                = AvbReadU32Be (Vbmeta + 28);
  HeaderOut->HashOffset                   = AvbReadU64Be (Vbmeta + 32);
  HeaderOut->HashSize                     = AvbReadU64Be (Vbmeta + 40);
  HeaderOut->SignatureOffset              = AvbReadU64Be (Vbmeta + 48);
  HeaderOut->SignatureSize                = AvbReadU64Be (Vbmeta + 56);
  HeaderOut->PublicKeyOffset              = AvbReadU64Be (Vbmeta + 64);
  HeaderOut->PublicKeySize                = AvbReadU64Be (Vbmeta + 72);
  HeaderOut->PublicKeyMetadataOffset      = AvbReadU64Be (Vbmeta + 80);
  HeaderOut->PublicKeyMetadataSize        = AvbReadU64Be (Vbmeta + 88);
  HeaderOut->DescriptorsOffset            = AvbReadU64Be (Vbmeta + 96);
  HeaderOut->DescriptorsSize              = AvbReadU64Be (Vbmeta + 104);
  HeaderOut->RollbackIndex                = AvbReadU64Be (Vbmeta + 112);
  HeaderOut->Flags                        = AvbReadU32Be (Vbmeta + 120);
  HeaderOut->RollbackIndexLocation        = AvbReadU32Be (Vbmeta + 124);
  for (int i = 0; i < 48; ++i) HeaderOut->ReleaseString[i] = (CHAR8)Vbmeta[128 + i];

  /* Sanity: header + auth + aux <= VbmetaSize. */
  UINT64 Total = (UINT64)GBL_AVB_VBMETA_HEADER_SIZE
                + HeaderOut->AuthenticationDataBlockSize
                + HeaderOut->AuxiliaryDataBlockSize;
  if (Total > VbmetaSize) return EFI_INVALID_PARAMETER;

  return EFI_SUCCESS;
}
EFI_STATUS EFIAPI
AvbParse_NextDescriptor (IN CONST UINT8 *AuxBlock, IN UINT64 AuxSize,
                         IN OUT UINT64 *Cursor,
                         OUT GBL_AVB_DESCRIPTOR_TAG *TagOut,
                         OUT CONST UINT8 **DescriptorOut,
                         OUT UINT64 *DescriptorLenOut)
{
  UINT64 Tag, NumBytesFollowing, Total;
  if (AuxBlock == NULL || Cursor == NULL || TagOut == NULL
      || DescriptorOut == NULL || DescriptorLenOut == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (*Cursor == AuxSize)                 return EFI_END_OF_MEDIA;
  if (*Cursor > AuxSize)                  return EFI_INVALID_PARAMETER;
  if (AuxSize - *Cursor < 16)             return EFI_INVALID_PARAMETER;
  Tag                = AvbReadU64Be (AuxBlock + *Cursor);
  NumBytesFollowing  = AvbReadU64Be (AuxBlock + *Cursor + 8);
  Total              = 16 + NumBytesFollowing;
  if (*Cursor + Total > AuxSize)          return EFI_INVALID_PARAMETER;
  *TagOut            = (GBL_AVB_DESCRIPTOR_TAG)Tag;
  *DescriptorOut     = AuxBlock + *Cursor;
  *DescriptorLenOut  = Total;
  *Cursor           += Total;
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
AvbParse_HashDescriptor (IN CONST UINT8 *Descriptor, IN UINT64 DescriptorLen,
                         OUT CONST UINT8 **PartitionNameOut, OUT UINT32 *PartitionNameLenOut,
                         OUT CONST UINT8 **DigestOut, OUT UINT32 *DigestLenOut)
{
  UINT32 NameLen, SaltLen, DigestLen;
  UINT64 BodyStart;
  if (Descriptor == NULL || PartitionNameOut == NULL || PartitionNameLenOut == NULL
      || DigestOut == NULL || DigestLenOut == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (DescriptorLen < 132)                return EFI_INVALID_PARAMETER;
  NameLen   = AvbReadU32Be (Descriptor + 56);
  SaltLen   = AvbReadU32Be (Descriptor + 60);
  DigestLen = AvbReadU32Be (Descriptor + 64);
  BodyStart = 132;
  if ((UINT64)NameLen + (UINT64)SaltLen + (UINT64)DigestLen + BodyStart > DescriptorLen) {
    return EFI_INVALID_PARAMETER;
  }
  *PartitionNameOut    = Descriptor + BodyStart;
  *PartitionNameLenOut = NameLen;
  *DigestOut           = Descriptor + BodyStart + NameLen + SaltLen;
  *DigestLenOut        = DigestLen;
  return EFI_SUCCESS;
}

/* Chain partition descriptor layout (libavb avb_chain_partition_descriptor.h):
     0   tag(u64)
     8   num_bytes_following(u64)
    16   rollback_index_location(u32)
    20   partition_name_len(u32)
    24   public_key_len(u32)
    28   reserved[64]
    92   partition_name | public_key
*/
EFI_STATUS EFIAPI
AvbParse_ChainPartitionDescriptor (IN CONST UINT8 *Descriptor, IN UINT64 DescriptorLen,
                                    OUT CONST UINT8 **PartitionNameOut, OUT UINT32 *PartitionNameLenOut,
                                    OUT CONST UINT8 **PublicKeyOut, OUT UINT32 *PublicKeyLenOut)
{
  UINT32 NameLen, PkLen;
  UINT64 BodyStart;
  if (Descriptor == NULL || PartitionNameOut == NULL || PartitionNameLenOut == NULL
      || PublicKeyOut == NULL || PublicKeyLenOut == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (DescriptorLen < 92)                 return EFI_INVALID_PARAMETER;
  NameLen = AvbReadU32Be (Descriptor + 20);
  PkLen   = AvbReadU32Be (Descriptor + 24);
  BodyStart = 92;
  if ((UINT64)NameLen + (UINT64)PkLen + BodyStart > DescriptorLen) {
    return EFI_INVALID_PARAMETER;
  }
  *PartitionNameOut    = Descriptor + BodyStart;
  *PartitionNameLenOut = NameLen;
  *PublicKeyOut        = Descriptor + BodyStart + NameLen;
  *PublicKeyLenOut     = PkLen;
  return EFI_SUCCESS;
}
