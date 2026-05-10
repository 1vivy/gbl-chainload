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

/* Stubs for tasks 2 & 3 — return -1 so test binary links. */
EFI_STATUS EFIAPI AvbParse_VbmetaHeader (CONST UINT8 *V, UINT64 S, GBL_AVB_VBMETA_HEADER *H) { (void)V; (void)S; (void)H; return -1; }
EFI_STATUS EFIAPI AvbParse_NextDescriptor (CONST UINT8 *A, UINT64 S, UINT64 *C, GBL_AVB_DESCRIPTOR_TAG *T, CONST UINT8 **D, UINT64 *L) { (void)A; (void)S; (void)C; (void)T; (void)D; (void)L; return -1; }
EFI_STATUS EFIAPI AvbParse_HashDescriptor (CONST UINT8 *D, UINT64 L, CONST UINT8 **N, UINT32 *NL, CONST UINT8 **DG, UINT32 *DGL) { (void)D; (void)L; (void)N; (void)NL; (void)DG; (void)DGL; return -1; }
EFI_STATUS EFIAPI AvbParse_ChainPartitionDescriptor (CONST UINT8 *D, UINT64 L, CONST UINT8 **N, UINT32 *NL, CONST UINT8 **PK, UINT32 *PKL) { (void)D; (void)L; (void)N; (void)NL; (void)PK; (void)PKL; return -1; }
