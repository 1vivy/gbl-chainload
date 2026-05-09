/** @file AblUnwrapLib.h
  Read the active abl partition and unwrap the embedded PE32+ image
  through Qualcomm's FV envelope: partition → ELF/FV header search →
  FFS files → PE32 section.

  Ported from gbl_root_canoe LinuxLoader.c (LoadAblFromPartition + the
  FindFirmwareVolume / FindPe32InFv / FindPe32InSectionStream helpers).
  Canoe ABL doesn't use the LZMA-compressed wrapper so the LZMA branch
  is intentionally omitted; if a future SoC needs it, add a section-type
  case in FindPe32InSectionStream.
**/
#ifndef GBL_CHAINLOAD_ABLUNWRAPLIB_H
#define GBL_CHAINLOAD_ABLUNWRAPLIB_H

#include <Uefi.h>

/** Load the named GPT partition and return the inner PE32+ payload that
    UEFI LoadImage can consume.

    @param[in]  PartitionName  Wide-string label of the GPT partition (e.g. L"abl_a")
    @param[out] OutPe          Caller-owned buffer containing PE32+ bytes
                               (pool-allocated, free with FreePool)
    @param[out] OutPeSize      Length of OutPe in bytes

    @retval EFI_SUCCESS         PE32+ extracted; OutPe valid, caller frees
    @retval EFI_NOT_FOUND       partition or FV/PE not found
    @retval EFI_NO_MEDIA        block-IO reports no media
    @retval EFI_OUT_OF_RESOURCES allocation failure
    @retval other               propagated read errors
**/
EFI_STATUS
EFIAPI
AblUnwrap_LoadFromPartition (
  IN  CHAR16  *PartitionName,
  OUT VOID   **OutPe,
  OUT UINT32  *OutPeSize
  );

#endif /* GBL_CHAINLOAD_ABLUNWRAPLIB_H */
