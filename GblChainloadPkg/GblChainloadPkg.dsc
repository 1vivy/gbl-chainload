## @file
#  GblChainload platform description — v2, modes 0 and 1.
#
#  Library mappings mirror QcomModulePkg.dsc so we get the same dep graph as
#  LinuxLoader.efi. Feature flags are passed as integer PCDs via GBL_MODE /
#  GBL_AUTO / GBL_DEBUG / GBL_VERBOSE env vars from scripts/build.sh.
##

[Defines]
  PLATFORM_NAME                  = GblChainloadPkg
  PLATFORM_GUID                  = 750169b4-e72b-4ba8-97db-e0c6adc6355f
  PLATFORM_VERSION               = 1.0
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/GblChainloadPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE
  SKUID_IDENTIFIER               = DEFAULT

  # Canoe board defaults — mirror gbl_root_canoe build_hooks_generic target.
  DEFINE BOARD_BOOTLOADER_PRODUCT_NAME    = canoe
  DEFINE VERIFIED_BOOT_ENABLED            = 1
  DEFINE VERIFIED_BOOT_LE                 = 0
  DEFINE AB_RETRYCOUNT_DISABLE            = 0
  DEFINE TARGET_BOARD_TYPE_AUTO           = 0
  DEFINE BUILD_USES_RECOVERY_AS_BOOT      = 0
  DEFINE DISABLE_PARALLEL_DOWNLOAD_FLASH  = 0
  DEFINE REMOVE_CARVEOUT_REGION           = 1
  DEFINE QSPA_BOOTCONFIG_ENABLE           = 1
  DEFINE USER_BUILD_VARIANT               = 0
  DEFINE AUTO_VIRT_ABL                    = 0
  DEFINE AUTO_LVGVM_ABL                   = 0
  DEFINE WEAR_OS                          = 0
  DEFINE ENABLE_LE_VARIANT                = 0
  DEFINE ENABLE_LV_ATOMIC_AB              = 0
  DEFINE ROOT_PARTLABEL_SUPPORT           = 0
  DEFINE SUPPORT_AB_BOOT_LXC              = 0
  DEFINE EARLY_ETH_ENABLED                = 0
  DEFINE EARLY_ETH_AS_DLKM               = 0
  DEFINE BOOTIMAGE_LOAD_VERIFY_IN_PARALLEL = 0
  DEFINE VERITY_LE                        = 0
  DEFINE HIBERNATION_SUPPORT_NO_AES       = 0
  DEFINE HIBERNATION_SUPPORT_AES          = 0
  DEFINE HIBERNATION_TZ_ENCRYPTION        = 0
  DEFINE HIBERNATION_SWAP_PARTITION_NAME  = 0
  DEFINE DISABLE_DTBO_PARTITION           = 0
  DEFINE APPEND_RAM_PARTITIONS_TO_MEM_NODE = 0
  DEFINE DDR_SUPPORTS_SCT_CONFIG          = 0
  DEFINE NAND_SQUASHFS_SUPPORT            = 0
  DEFINE NAND_UBI_VOLUME_FLASHING_ENABLED = 0
  DEFINE USE_DUMMY_BCC                    = 0
  DEFINE BASE_ADDRESS                     = 0
  DEFINE TARGET_LINUX_BOOT_CPU_ID         = 0
  DEFINE ENABLE_EARLY_SERVICES            = 0
  DEFINE KERNEL_LOAD_ADDRESS              = 0
  DEFINE KERNEL_SIZE_RESERVED             = 0
  DEFINE DISABLE_KERNEL_PROTOCOL          = 0
  DEFINE TARGET_SUPPORTS_EARLY_USB_INIT   = 0
  # BootLib's UpdateCmdLine.c hard-references INIT_BIN; value is cosmetic.
  DEFINE INIT_BIN                         = /init
  DEFINE GBL_CHAINLOAD_VERSION            = 2.0

  # v2 mode flags — overridden at build time via -D from build-inside-docker.sh.
  # These are integer flags, not legacy mode-name strings.
  DEFINE GBL_MODE                         = 1
  DEFINE GBL_AUTO                         = 0
  DEFINE GBL_DEBUG                        = 0
  DEFINE GBL_VERBOSE                      = 0

  # Build name — single string identifier substituted into log banner,
  # FastbootMenu display, and the build-name getvar. build-inside-docker.sh
  # constructs the suffixed form (e.g. mode-1-auto-debug-verbose).
  DEFINE GBL_BUILD_NAME                   = mode-unknown

################################################################################
# Library mappings — mirrors QcomModulePkg.dsc [LibraryClasses.*]
################################################################################
[LibraryClasses.common]
  BaseStackCheckLib|MdePkg/Library/BaseStackCheckLib/BaseStackCheckLib.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  CacheMaintenanceLib|ArmPkg/Library/ArmCacheMaintenanceLib/ArmCacheMaintenanceLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  DxeServicesTableLib|MdePkg/Library/DxeServicesTableLib/DxeServicesTableLib.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  FdtLib|EmbeddedPkg/Library/FdtLib/FdtLib.inf
  LibUfdt|EmbeddedPkg/Library/LibUfdt/LibUfdt.inf
  TimerLib|ArmPkg/Library/ArmArchTimerLib/ArmArchTimerLib.inf
  ArmGenericTimerCounterLib|ArmPkg/Library/ArmGenericTimerPhyCounterLib/ArmGenericTimerPhyCounterLib.inf
  Zlib|QcomModulePkg/Library/zlib/zlib.inf
  BaseMemoryLibOptDxe|MdePkg/Library/BaseMemoryLibOptDxe/BaseMemoryLibOptDxe.inf
  ## Route DEBUG() through gST->ConOut so LogFsLib's DebugSink can mirror it.
  DebugLib|MdePkg/Library/UefiDebugLibConOut/UefiDebugLibConOut.inf
  ReportStatusCodeLib|MdeModulePkg/Library/DxeReportStatusCodeLib/DxeReportStatusCodeLib.inf
  DebugPrintErrorLevelLib|MdeModulePkg/Library/DxeDebugPrintErrorLevelLib/DxeDebugPrintErrorLevelLib.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  PerformanceLib|MdeModulePkg/Library/DxePerformanceLib/DxePerformanceLib.inf
  AvbLib|QcomModulePkg/Library/avb/AvbLib.inf
  AesLib|QcomModulePkg/Library/aes/AesLib.inf
  Lz4Lib|QcomModulePkg/Library/lz4/lib/Lz4Lib.inf
  ## Used by AblUnwrapLib to decompress LZMA-wrapped FFS sections in canoe ABL.
  LzmaDecompressLib|MdeModulePkg/Library/LzmaCustomDecompressLib/LzmaCustomDecompressLib.inf
  ## Our libraries.
  AblUnwrapLib|GblChainloadPkg/Library/AblUnwrapLib/AblUnwrapLib.inf
  LogFsLib|GblChainloadPkg/Library/LogFsLib/LogFsLib.inf
  DynamicPatchLib|GblChainloadPkg/Library/DynamicPatchLib/DynamicPatchLib.inf
  ProtocolHookLib|GblChainloadPkg/Library/ProtocolHookLib/ProtocolHookLib.inf
  AvbParseLib|GblChainloadPkg/Library/AvbParseLib/AvbParseLib.inf

[LibraryClasses.AARCH64]
  ArmLib|ArmPkg/Library/ArmLib/ArmBaseLib.inf
  NULL|ArmPkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf
  OpenDice|QcomModulePkg/Library/OpenDice/open-dice.inf
  CompilerIntrinsicsLib|ArmPkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf

[LibraryClasses.common.UEFI_APPLICATION]
  ReportStatusCodeLib|MdeModulePkg/Library/DxeReportStatusCodeLib/DxeReportStatusCodeLib.inf
  ExtractGuidedSectionLib|MdePkg/Library/DxeExtractGuidedSectionLib/DxeExtractGuidedSectionLib.inf

################################################################################
# Build options — mirrors QcomModulePkg.dsc [BuildOptions.common]
################################################################################
[BuildOptions.common]
  GCC:*_*_*_ARCHCC_FLAGS  = -Wno-shift-negative-value -fstack-protector-all -Wno-varargs -fno-common -Wno-misleading-indentation -Wno-unknown-warning-option
  GCC:*_*_*_DLINK_FLAGS = -Wl,-Ttext=0x0
  GCC:*_*_*_CC_FLAGS = -DZ_SOLO
  GCC:*_*_*_CC_FLAGS = -DPRODUCT_NAME=\"$(BOARD_BOOTLOADER_PRODUCT_NAME)\"
  GCC:*_*_*_CC_FLAGS = -DGBL_CHAINLOAD_VERSION=\"$(GBL_CHAINLOAD_VERSION)\"

  # v2 mode flags — integer-based, no legacy mode strings.
  GCC:*_*_*_CC_FLAGS = -DGBL_MODE=$(GBL_MODE)
  GCC:*_*_*_CC_FLAGS = -DGBL_AUTO=$(GBL_AUTO)
  GCC:*_*_*_CC_FLAGS = -DGBL_DEBUG=$(GBL_DEBUG)
  GCC:*_*_*_CC_FLAGS = -DGBL_VERBOSE=$(GBL_VERBOSE)
  GCC:*_*_*_CC_FLAGS = -DGBL_BUILD_NAME=\"$(GBL_BUILD_NAME)\"

  # Workarounds for this Qualcomm edk2 fork against modern Ubuntu GCC:
  #  - __FORTIFY_SOURCE: BaseLib.h:148 macro space-bug workaround
  #  - -fno-stack-protector: Ubuntu gcc default-on stack-protector breaks -nostdlib
  GCC:*_*_AARCH64_CC_FLAGS = -D__FORTIFY_SOURCE -fno-stack-protector -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-error=unused-function -Wno-error=array-parameter -DGBL_EXPERIMENTAL_FASTBOOT_CMDS=1

  !if $(VERIFIED_BOOT_LE)
      GCC:*_*_*_CC_FLAGS = -DVERIFIED_BOOT_LE
  !endif
  !if $(VERIFIED_BOOT_ENABLED)
      GCC:*_*_*_CC_FLAGS = -DVERIFIED_BOOT_ENABLED
  !endif
  !if $(USER_BUILD_VARIANT) == 0
      GCC:*_*_*_CC_FLAGS = -DENABLE_UPDATE_PARTITIONS_CMDS -DENABLE_BOOT_CMD -DENABLE_DEVICE_CRITICAL_LOCK_UNLOCK_CMDS
  !else
      GCC:*_*_*_CC_FLAGS = -DUSER_BUILD_VARIANT
  !endif
  !if $(REMOVE_CARVEOUT_REGION) == 1
      GCC:*_*_*_CC_FLAGS = -DREMOVE_CARVEOUT_REGION
  !endif
  !if $(QSPA_BOOTCONFIG_ENABLE) == 1
      GCC:*_*_*_CC_FLAGS = -DQSPA_BOOTCONFIG_ENABLE
  !endif
  # BootLib hard-references INIT_BIN; pass it through always.
  GCC:*_*_*_CC_FLAGS = -DINIT_BIN='"$(INIT_BIN)"'

################################################################################
# PCDs
################################################################################
[PcdsFixedAtBuild.common]
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x2f
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000042
  gEfiMdePkgTokenSpaceGuid.PcdReportStatusCodePropertyMask|0x06

################################################################################
# Components
################################################################################
[Components.common]
  GblChainloadPkg/Application/GblChainload/GblChainload.inf {
    <LibraryClasses>
      DxeServicesTableLib|MdePkg/Library/DxeServicesTableLib/DxeServicesTableLib.inf
      UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
      UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
      CacheMaintenanceLib|ArmPkg/Library/ArmCacheMaintenanceLib/ArmCacheMaintenanceLib.inf
      Zlib|QcomModulePkg/Library/zlib/zlib.inf
      ArmLib|ArmPkg/Library/ArmLib/ArmBaseLib.inf
      BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
      DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
      HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
      PerformanceLib|MdeModulePkg/Library/DxePerformanceLib/DxePerformanceLib.inf
      DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
      FdtLib|EmbeddedPkg/Library/FdtLib/FdtLib.inf
      LibUfdt|EmbeddedPkg/Library/LibUfdt/LibUfdt.inf
      ArmSmcLib|ArmPkg/Library/ArmSmcLib/ArmSmcLib.inf
      BootLib|QcomModulePkg/Library/BootLib/BootLib.inf
      StackCanary|QcomModulePkg/Library/StackCanary/StackCanary.inf
      FastbootLib|QcomModulePkg/Library/FastbootLib/FastbootLib.inf
      AvbLib|QcomModulePkg/Library/avb/AvbLib.inf
      OpenDice|QcomModulePkg/Library/OpenDice/open-dice.inf
      AesLib|QcomModulePkg/Library/aes/AesLib.inf
      UbsanLib|QcomModulePkg/Library/UbsanLib/UbsanLib.inf
      Lz4Lib|QcomModulePkg/Library/lz4/lib/Lz4Lib.inf
      LoadFVLib|QcomModulePkg/Library/LoadFVLib/LoadFVLib.inf
      LogFsLib|GblChainloadPkg/Library/LogFsLib/LogFsLib.inf
      AblUnwrapLib|GblChainloadPkg/Library/AblUnwrapLib/AblUnwrapLib.inf
      DynamicPatchLib|GblChainloadPkg/Library/DynamicPatchLib/DynamicPatchLib.inf
      ProtocolHookLib|GblChainloadPkg/Library/ProtocolHookLib/ProtocolHookLib.inf
  }

  # Throwaway probe — answers Q1 (UefiInfoBlk HOB layout) and Q5 (log flush timing).
  # Delete after on-device capture.
  GblChainloadPkg/Application/UartProbeA/UartProbeA.inf {
    <LibraryClasses>
      UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
      UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
      BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
      HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
      TimerLib|ArmPkg/Library/ArmArchTimerLib/ArmArchTimerLib.inf
      DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
      DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  }
