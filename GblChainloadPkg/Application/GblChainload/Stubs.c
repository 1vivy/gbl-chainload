/** @file
  Stubs for BootLib / FastbootLib callbacks normally provided by
  QcomModulePkg/Application/LinuxLoader/LinuxLoader.c.

  Step 1b never reaches kernel-boot logic, so these stubs return safe
  defaults. Real implementations land later as bring-up needs them.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/LinuxLoaderLib.h>
#include <Library/BootLinux.h>

/* Boot-state globals BootLib expects to find. */
BccParams_t BccParamsRecvdFromAVB = {{0}};

BOOLEAN
IsABRetryCountUpdateRequired (VOID)
{
  /* Return FALSE so the A/B retry counter is never decremented from
   * within our app's code path. We don't reach normal kernel boot. */
  return FALSE;
}

UINT32
GetBootDeviceType (VOID)
{
  /* Return EFI_MAX_FLASH_TYPE = "unknown / not yet queried"; callers
   * will fall through to the reading-from-NV-var path. */
  return EFI_MAX_FLASH_TYPE;
}
