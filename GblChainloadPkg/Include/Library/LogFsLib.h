/** @file LogFsLib.h
  Mount the device's `logfs` partition (located by GPT label) and rotate
  UefiLog1.txt → UefiLogSaved{0..4}.txt across boots so BDS log history
  is preserved.

  The GblChainload_BootN.txt auto-mirror mechanism has been removed.
  UefiLog<N>.txt — written by QCOM's BSP-installed status-code handler
  (gBS->ReportStatusCode → handler → UART log buffer → BDS flush, the
  same path PeiDxeDebugLibReportStatusCode targets) — is the sole
  persistent log destination.
**/
#ifndef GBL_CHAINLOAD_LOGFSLIB_H
#define GBL_CHAINLOAD_LOGFSLIB_H

#include <Uefi.h>

/** Mount logfs and rotate UefiLog1.txt → UefiLogSaved{0..4}.txt.

    @retval EFI_SUCCESS    logfs mounted and rotation attempted
    @retval EFI_NOT_FOUND  logfs partition absent
    @retval other          partition lookup / mount errors propagated
**/
EFI_STATUS
EFIAPI
LogFsInit (VOID);

/** Close the logfs root volume handle. Call before any chain-load handoff
    so the next EFI image can mount the partition without finding it bound
    to our driver instance. No-op if LogFsInit was not called or already
    closed. **/
EFI_STATUS
EFIAPI
LogFsClose (VOID);

#endif /* GBL_CHAINLOAD_LOGFSLIB_H */
