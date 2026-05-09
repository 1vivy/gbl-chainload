# 2026-05-08 — repo audit, fastboot/logfs, watchdog, EFISP, T2 readiness

## Scope

Host-side review only. No Ghidra annotations were applied and no device run was
performed in this session.

## Audit findings to carry forward

- High: `QseecomHook.c` logs `AppName` with `%a` and uses `AsciiStrCmp()` before
  proving bounded NUL termination. OplusSec uses a 16-byte GUID-like binary TA
  name, so future fix should classify bounded/binary first and only print/strcmp
  ASCII names after a bounded printable/NUL check.
- High: `AblUnwrapLib.c` FV/section parsing needs remaining-size-aware helpers;
  GUID-defined section size math can underflow if malformed input supplies a bad
  data offset.
- High: EBS/logfs lifecycle is intentionally long-lived for hook logging, but
  `HookedExitBootServices()` currently only flushes. For a branch/device run,
  test removing the debug sink and closing logfs immediately before tail-calling
  original EBS.
- Medium: `BootFlow.c` should unload `ImageHandle` if `StartImage()` returns.
- Medium: `LogFsInit()` leaves `gLogFsRoot` open if post-GBL log open fails;
  retry handling should close or reuse the existing root rather than remounting.
- Low: `tests/runall.sh` comment says stop at first failure, but the loop
  accumulates failures and continues.

## Fastboot/logfs retrieval

- Current path: `fastboot oem get-staged logfs` streams raw `logfs` partition
  bytes over `INFO h=<hex>` lines; `scripts/pull-logfs.sh` reassembles to
  `logfs.img`.
- Device implementation: `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c`
  `CmdOemGetStagedLogfs()` locates `logfs` by GPT label and uses raw
  `EFI_BLOCK_IO_PROTOCOL.ReadBlocks`, avoiding FAT/SimpleFS dependency.
- Recommendation: keep current INFO-stream fallback because it avoids staging the
  whole partition in RAM. A more standard future UX could stage bytes for the
  protocol `upload`/host `get_staged` path, but that trades simplicity for RAM
  pressure and more FastbootLib surgery.

## Watchdog / Phoenix

- Current FastbootLib already has a canoe-specific watchdog defense around
  `FastbootCmdsInit()`: it hooks `gBS->SetWatchdogTimer` to clamp timeout to 0,
  locates `QcomWDog`, calls `Disable()`, `SetBiteTimeout(0)`, and arms a 5s pet
  event via `ForceWDogPet()`.
- Comments explicitly call out Phoenix re-arming a 60s watchdog from OEM code
  with no source visibility. If resets still occur, the next device test should
  confirm these log lines appear before the 60s reset and whether the 5s pet
  callback fires repeatedly.

## EFISP flashing

- Singular/non-slotted flashing exists in raw/sparse/ubi/meta flash handlers, but
  the multislot helper can still mutate a requested partition name to current
  slot suffix before `PartitionGetInfo()`.
- `GetPartitionHasSlot()` appends `GetCurrentSlotSuffix()` when
  `GetPartitionIndex(base)` fails; `HandleRawImgFlash()` then tries only the
  mutated name and returns. `CmdErase()` has a better fallback pattern: try
  suffixed partitions, then the base partition.
- Recommended fix belongs in an `edk2/` branch only with explicit approval:
  preserve the original partition name, try base `PartitionGetInfo()` first, and
  only then try suffixed variants; never assume a missing index means the target
  is slotted.

## T1 readiness / T2 next

- T1 mode-debug is sufficient to start T2.1 now. It is not sufficient to start
  T6 implementation.
- Start T2.1 by writing `docs/re/abl-avb-flow.md` from upstream source anchors:
  `VerifiedBoot.c` `LoadImageAndAuth`, `LoadImageAndAuthVB2`, requested
  partitions, BootState/KM/VBH handoff, vbmeta digest formula, and
  `KeymasterClient.c` ROT/BootState wire construction.
- Before T6, resolve SPSS vtable/KeyMint sharing, canoe boot-color enum mismatch,
  stock locked pubkey baseline, and consolidate T5.1.

## Branch/device-test recommendation

Do not mix the boot-critical changes above into the current branch piecemeal.
Create a short-lived audit-fixes branch for QSEE bounded-name logging, logfs/EBS
lifecycle, AblUnwrap hardening, and EFISP flash fallback; build it and run
`./scripts/test-device-automatic.sh dist/gbl-chainload.efi`, then compare pulled
logfs for watchdog, hook, and EBS behavior before merging.

## Follow-up implementation notes

- Branch used: `audit-fixes-baseline`.
- Baseline and post-change host checks passed:
  - `./scripts/build.sh canoe DEBUG`
  - `./tests/runall.sh`
- Monitored automatic device test passed with
  `./scripts/test-device-automatic.sh dist/gbl-chainload.efi`.
  Log directory:
  `logs/20260508-011621_auto_gbl-chainload_v0.1-step1d/`.
- Device monitor evidence captured USB state transitions in
  `20260508-011621_device-monitor.log`; foreground fastboot calls now use a
  shared lock so the monitor does not race `stage`, `oem boot-efi`, or
  `oem get-staged logfs`.
- Logfs evidence from the successful run:
  - `AblUnwrap` resolved `abl_a`, found FV at `0x1000`, and extracted a
    `0xBE000` PE/TE.
  - mandatory `patch1-efisp-recursion` applied successfully.
  - QSEE app logging produced bounded ASCII names (`keymaster`,
    `secretkeeper_a`) with no binary-name `%a` logging.
  - FastbootLib watchdog hardening logged SetWatchdog clamp, QcomWDog disable,
    `SetBiteTimeout(0)`, and 5s pet event armed.

## EFISP / singular partition flashing verification

- Source + Ghidra cross-check of `LinuxLoader_infiniti.efi` confirms raw flash
  flow: `HandleRawImgFlash` calls `PartitionHasMultiSlot("boot")`, then
  `GetPartitionHasSlot(requested)`, then exactly one `PartitionGetInfo()` on the
  possibly-mutated name before size-check/write.
- For a literal singular partition that exists in GPT, the fixed
  `GetPartitionHasSlot()` leaves the requested base name unchanged. It now only
  mutates to `<name>_a/_b` if the base is absent and the suffixed candidate
  actually exists.
- Non-writing fastboot verification on current canoe fastboot showed no
  `partition-size:efisp`, `efisp_a`, or `efisp_b` variables; `getvar all` listed
  `uefi_a`/`uefi_b` and many A/B partitions instead. So the singular EFISP
  claim does not match this current variable inventory; verify exact partition
  label before any flash.

## BCB controls

- Added FastbootLib OEM commands for future sibling/testbench builds:
  - `fastboot oem bcb-recovery` → write `boot-recovery` to misc BCB.
  - `fastboot oem bcb-fastboot` → write `boot-fastboot` to misc BCB.
  - `fastboot oem bcb-clear` → zero `struct RecoveryMessage` in misc.
- `scripts/test-device-automatic.sh` now accepts:
  - `--no-return` / `GBL_TEST_RETURN_TO_FASTBOOT=0` to leave the device in its
    captured adb state instead of rebooting back into sibling FastbootLib.
  - `--bcb=recovery|fastboot|clear|none` / `GBL_TEST_BCB=...` to request BCB
    setup before `oem boot-efi`. This requires the currently-running FastbootLib
    (sibling/testbench) to include the new OEM commands.

## USB/FAT external dump architecture

- External USB/FAT dumping is feasible as a separate architecture track, not a
  quick replacement for current fastboot INFO retrieval.
- Best path: add a generic “load file/driver from any mounted SimpleFS” helper
  modeled on `BootESP.c`, then support FV/partition/USB search paths.
- USB mass-storage requires host-side USB stack drivers (`UsbBusDxe`, a platform
  host controller driver, `UsbMassStorageDxe`, `FatDxe`) in addition to the
  existing fastboot USB device-mode path.
- Treat Project-Silicium/Device-Binaries or USB-carried DXE drivers as a debug
  feature gated by explicit physical/debug action; arbitrary DXE loading expands
  the trust boundary substantially.
- Keep `oem get-staged logfs`/future fastboot `upload` as the primary near-term
  retrieval path because it is lower-risk and already works in FastbootLib.

## Assisted recovery retry after watchdog timeout

- After a reported watchdog timeout, the device returned to fastboot as
  `8549c105 fastboot`.
- Retried with:
  `./scripts/test-device-automatic.sh --no-return dist/gbl-chainload.efi`.
- Run succeeded and left the device in adb recovery state. Log directory:
  `logs/20260508-012828_auto_gbl-chainload_v0.1-step1d/`.
- Stage/boot-efi evidence:
  - `fastboot stage` succeeded for `618496` bytes.
  - `oem boot-efi` printed `loading staged 618496 bytes`; USB drop was treated
    as expected `StartImage` handoff.
  - adb came up as `state=recovery`; `getprop ro.bootmode`/state output showed
    recovery, slot `_a`.
- Device monitor evidence:
  - started in fastboot, then fastboot was busy during stage/boot-efi, then USB
    disappeared during handoff; log capture completed once adb recovery came up.
- Logfs evidence:
  - `GblChainload_Boot3.txt` shows current staged payload entry, key window, no
    key pressed, ABL `_a` unwrap, `patch1-efisp-recursion` and orange-screen
    patches applied, hooks installed, and handoff to patched ABL.
  - QSEE names were bounded ASCII: `keymaster`, `secretkeeper_a`.
  - EBS hook fired and recorded FDT candidates before Linux handoff.
  - Watchdog hardening remained active in FastbootLib logs:
    SetWatchdog hook, QcomWDog disable, `SetBiteTimeout(0)`, and 5s pet event.
- Kernel/recovery evidence:
  - `dmesg.txt` command line includes `oplusboot.mode=recovery` and
    `oplus_ftm_mode.oplus_ftm_mode=ftmrecovery`.
  - Search found normal PMIC/PS_HOLD hard-reset records and Linux watchdog module
    initialization, but no obvious fresh bootloader watchdog-bite signature in
    the successful retry logs.

## BCB recovery command validation from freshly-built payload

- Build policy was tightened after stale-object suspicion: `scripts/build.sh`
  now unconditionally removes `Build/` and `dist/gbl-chainload.efi`; legacy
  `--no-clean`/`--incremental` exits with an error.
- Clean host validation passed after the build-script change:
  - script syntax checks for build/device scripts
  - `./scripts/build.sh canoe DEBUG`
  - `./tests/runall.sh`
- The sibling image currently flashed did not yet expose the new BCB OEM
  commands (`oem bcb-recovery` returned `unknown command` there), so the command
  was tested from the freshly-built staged payload's own FastbootLib.
- Successful retry log directory:
  `logs/20260508-014221_bcb_recovery_payload_fastboot_retry/`.
- Procedure/evidence:
  - staged fresh `dist/gbl-chainload.efi` (`618496` bytes)
  - `oem boot-efi` reached payload handoff (`loading staged 618496 bytes`, USB
    drop)
  - user held VolDown during the payload key window so the payload entered its
    FastbootLib
  - `oem bcb-recovery` returned `OKAY`
  - `fastboot reboot` returned `OKAY`
  - adb came back as `recovery`; `recovery.props` showed bootmode `recovery` and
    slot `_a`
- Logfs/bootloader evidence:
  - `GblChainload_Boot2.txt` contains the payload FastbootLib processing
    `oem bcb-recovery`.
  - `GblChainload_Boot3.txt` and `UefiLog1.txt` show
    `bcb="boot-recovery"`, `recovery=1`, and routing to recovery through
    patched ABL.
  - `cmdline`/`dmesg.txt` confirm `oplusboot.mode=recovery` and
    `oplus_ftm_mode.oplus_ftm_mode=ftmrecovery`.
  - patched ABL path still shows `patch1-efisp-recursion OK` and
    `patch7-orange-screen OK`.
- Shell tooling fix found during manual one-liner tests: renamed the local
  variable in `device_monitor_fastboot()` from `status` to `rc` because `zsh`
  exposes `status` as read-only when sourced from an interactive shell. The
  scripts remain bash-targeted, but the helper is safer when sourced manually.
- ADB wait behavior was adjusted to poll `adb devices` for explicit
  `device|recovery|sideload|rescue` states instead of relying on
  `adb wait-for-device`/`adb get-state`, which can be inconvenient for recovery
  enumeration.

## Improved EBS-only FDT probe validation

- Built and ran `dist/mode-debug-ebs-fdt-probe-only.efi` after hardening the EBS
  wrapper/probe path:
  - restore the `gBS->ExitBootServices` slot and CRC before any EBS-time logging;
  - include `EfiConventionalMemory` in the probe memory types;
  - skip oversized descriptors instead of stopping the scan;
  - rank FDT candidates by `/chosen/bootargs` token matches and initrd range.
- Device validation capture:
  `logs/20260508-134430_auto_mode-debug-ebs-fdt-probe-only_vb4a4ee5/`.
- Result: booted recovery and returned to sibling FastbootLib successfully.
  Recovery userspace confirmed normal recovery state:
  - `/proc/cmdline`: `oplusboot.mode=recovery`, `ftmrecovery`, `bootconfig`,
    `rootwait`, `init=/init`;
  - `/proc/bootconfig`: `androidboot.mode = "recovery"`, `dtb_idx = "8"`,
    `dtbo_idx = "6"`, unlocked/orange vbmeta state;
  - `dmesg.txt`: `Load bootconfig: 2692 bytes 136 nodes` and initrd freed.
- EBS hook evidence in `logfs/UefiLog1.txt:160-184`:
  - wrapper fired after `Update Device Tree total time` and before original EBS;
  - slot restoration completed before probe/logging;
  - probe found 16 FDT-shaped blobs but every candidate had `chosen=-1` or
    `chosen=-11`;
  - large `Conventional`/`BootServicesData` descriptors were skipped by the global
    512 MiB scan cap;
  - final summary: `regions=49 bytes=479469568 fdt-hits=16 selected=0 score=-1`.
- Conclusion: the improved EBS probe is boot-safe but still not a reliable final
  FDT/initrd/bootconfig locator. EBS should remain a lifecycle/safety hook. For
  reliable mutation, pivot to a final-callsite helper around the BootLib
  `UpdateDeviceTree()` / bootconfig serialization cluster instead of broad EBS
  memory discovery.

## Ghidra final-callsite anchor work

- User preference: use Ghidra for RE/correlation; use online search only if a
  tool/library issue needs investigation and ask before requiring user action.
- Ghidra project: `gbl_root_canoe`.
- Active/open programs included `LinuxLoader_infiniti.efi`, `LinuxLoader.efi`,
  `OplusSecurityDxe.efi`; imported ABL containers under `/tests`:
  - `/tests/abl.img` from the RegionalHybrid flasher path;
  - `/tests/001_myron_abl.elf`;
  - `/tests/002_infiniti_abl.elf`.
- Applied Ghidra annotations in `LinuxLoader_infiniti.efi`:
  - `00037d9c` renamed `UpdateDeviceTree_Qcom` with a plate comment describing
    final FDT mutation of `/chosen/bootargs` and `linux,initrd-*`.
  - `0000da40` renamed `BootLinux_Qcom` with a plate comment describing the
    Android BootLinux path and evidence.
  - `00015cb4` disassembly comment marks the final `UpdateDeviceTree_Qcom`
    callsite and argument registers/stack sources.
  - Program saved after annotations.
- Applied Ghidra annotations in ABL containers:
  - `abl.img:9fa00028` and `002_infiniti_abl.elf:9fa00028` labeled
    `EmbeddedFirmwareVolumeHeader` and commented as the `_FVH` container layer
    that must be unwrapped before PE-level patch signatures are visible.
- Callsite bytes in `LinuxLoader_infiniti.efi`:
  - `00015ca4: ldr x0, [sp,#0x578]`
  - `00015ca8: mov x2, x20`
  - `00015cac: ldr x1, [sp,#0x5e0]`
  - `00015cb0: mov w3, w21`
  - `00015cb4: bl UpdateDeviceTree_Qcom`
  - `00015cb8: cbz x0, ...`
- Stable PE-level anchor pattern with BL immediate wildcarded:
  `e0 bf 42 f9 e2 03 14 aa e1 f3 42 f9 e3 03 15 2a ?? ?? ?? 94 c0 00 00 b4`.
- Ghidra search results:
  - `LinuxLoader_infiniti.efi`: one hit at `00015ca4`.
  - `LinuxLoader.efi`: one hit at `00019744`.
- Container validation used existing extractor:
  `gbl_root_canoe/tools/extractfv.c` compiled to `/tmp/gbl_extractfv` and run on
  the provided ABL ELF containers. Extracted `LinuxLoader.efi` pattern hits:
  - RegionalHybrid `abl.img`: one hit at file offset `0x15cac`.
  - `tests/002_infiniti_abl.elf`: one hit at file offset `0x15ca4`.
  - `tests/001_myron_abl.elf`: one hit at file offset `0x19744`.
- Raw ABL ELF containers do not contain the AArch64 callsite pattern directly;
  Ghidra sees the embedded FV header at `9fa00028`. This matches the runtime
  `AblUnwrapLib` flow: extract/decompress nested LinuxLoader PE, then call
  `DynamicPatch_Apply()` on that PE buffer.
- Next decision before coding mutation: whether to first add an anchor-only host
  validation/pseudo-patch, or design a real callsite redirect/code-cave helper.
  Do not mutate until helper storage and return/control-flow are designed.

## Mode consolidation validation

- Branch used: `consolidate-modes` from `audit-fixes-baseline`.
- Added explicit build modes wired from scripts → DSC macros → C preprocessor
  defines:
  - `--mode auto-debug` / `--mode sibling` → `AUTO_DEBUG_MODE` →
    `dist/auto-debug.efi`.
  - `--mode mode-debug` → `MODE_DEBUG` → `dist/mode-debug.efi`.
  - placeholders for future `minimal` and `mode-1` compile selections.
- `Entry.c` now enforces exactly one build mode at compile time. `AUTO_DEBUG_MODE`
  keeps the minimal sibling behavior: default to FastbootLib, VolUp/BCB recovery
  chain-loads patched ABL to recovery, VolDown/BCB bootloader chain-loads patched
  ABL to OEM bootloader fastboot. `MODE_DEBUG` keeps the key-window + hook-debug
  chain-load behavior.
- `BootFlow.c` only includes/calls `ProtocolHookLib` in hook modes
  (`MODE_DEBUG`/future `MODE_1`), keeping `AUTO_DEBUG_MODE` minimal.
- Fixed one consolidation compile issue: `WaitForVolKey()` is `MODE_DEBUG`-only,
  while auto-debug helpers and `RunModeDebug()` have separate preprocessor
  guards.
- Host validation passed:
  - `./scripts/build.sh canoe DEBUG --mode mode-debug && ./tests/runall.sh`
    produced `dist/mode-debug.efi` (`618496` bytes) and all tests passed.
  - `./scripts/build.sh canoe RELEASE --mode auto-debug && ./tests/runall.sh`
    produced `dist/auto-debug.efi` (`536576` bytes) and all tests passed.
- Note: the last validation build leaves `dist/gbl-chainload.efi` pointing at the
  selected `auto-debug` artifact, as intended for existing scripts.
- Post-review adjustment: auto-debug escape routing no longer halts forever if
  patched ABL cannot be started. `TryEscapeChainLoad()` now logs the returned
  status, clears the BCB command field with `WriteRecoveryMessage("")`, flushes,
  and falls back to the default FastbootLib path so the permanent sibling remains
  recoverable. Revalidated with
  `./scripts/build.sh canoe RELEASE --mode auto-debug && ./tests/runall.sh`.
- Follow-up logging design: replace direct `Print()` policy with a real
  severity-aware logging layer. Desired behavior is for log/debug output to be
  mirrored to screen by mode/severity: `EFI_ERROR` always visible, likely
  `EFI_WARNING` visible for debug/user-interrupt cases, and `EFI_INFO` gated to
  debug modes. Numbered modes should rely on this instead of ad-hoc screen-print
  suppression.
- Remaining device-confirmation item: `AUTO_DEBUG_MODE` preserves the existing
  `bootonce-bootloader` BCB command for VolDown/bootloader escape because the
  in-tree `RECOVERY_BOOT_FASTBOOT` value (`boot-fastboot`) appears to mean
  recovery/userspace fastbootd. Confirm on actual canoe ABL before relying on BCB
  bootloader routing in a flashed sibling.
- Device confirmation: with the flashed sibling FastbootLib active, clearing BCB,
  rebooting, and holding VolDown during the sibling entry window returned to stock
  OEM bootloader fastboot (`3C15AT003ZB00000 fastboot`). Stock getvars worked
  (`product: canoe`, `current-slot: a`) and sibling-only `oem bcb-clear` returned
  `unknown command`, confirming this was not the sibling FastbootLib. This validates
  the existing `bootonce-bootloader` VolDown escape path when the key window is hit.

## Payload recovery/default flow

- Desired operator flow:
  - flashed `AUTO_DEBUG_MODE`/sibling defaults to our FastbootLib
  - host stages `mode-debug`/future payload and runs `oem boot-efi`
  - payload default path continues into recovery for adb-side log collection
  - payload user interrupt enters payload FastbootLib so host can pull raw logfs
    with `oem get-staged logfs`
- LinuxLoader recovery source check:
  - `Application/LinuxLoader/LinuxLoader.c` sets `BootIntoRecovery` from VolUp,
    `RECOVERY_MODE`, or `RecoveryInit()` and then passes it in `BootInfo` to
    `LoadImageAndAuth()`/`BootLinux()`.
  - `BootLib/Recovery.c` makes `RecoveryInit()` set recovery for BCB
    `boot-recovery` and dynamic-partition `boot-fastboot`.
  - `BootLib/BootLinux.c` chooses recovery images / recovery ramdisk behavior
    based on `Info.BootIntoRecovery`.
- Practical payload approach for now: do not reboot and do not use FastbootLib
  `continue`. `mode-debug` writes BCB `boot-recovery`, flushes, then starts the
  patched ABL via `BootFlowChainLoad()`. The patched ABL's LinuxLoader path sees
  `boot-recovery` through `RecoveryInit()` and enters recovery normally.
- `mode-debug` user interrupt path now closes logfs and enters payload
  FastbootLib, making raw `logfs` pulls less likely to see stale SimpleFS
  metadata.
- Logfs write policy was adjusted away from per-line flushing: normal writes are
  appended and flushed when dirty bytes exceed 4 KiB or at explicit transition
  points (`LogFsFlush()`/`LogFsClose()` before Fastboot handoff, BootFlow handoff,
  and EBS hook). Blob writes still flush/close immediately.
- `UefiLog1.txt` archiving now also runs in staged payload context. FV-loaded
  primary GBL keeps the original destructive rotate behavior (archive then delete
  source so BDS starts fresh). Staged payloads snapshot `UefiLog1.txt` into the
  `UefiLogSavedN.txt` ring without deleting the source, preserving bootchain and
  sibling/FastbootLib context while avoiding the prior risk of deleting a file the
  outer loader may still have open.
- Device validation from the old flashed sibling:
  - `./scripts/test-device-automatic.sh --no-return dist/mode-debug.efi` staged
    `618496` bytes, `oem boot-efi` dropped USB on `StartImage`, and adb came up
    in recovery.
  - log directory: `logs/20260508-080413_auto_mode-debug_v0.1-step1d/`.
  - `GblChainload_Boot2.txt` shows old sibling FastbootLib handled
    `download:00097000` + `oem boot-efi`, then the staged `MODE_DEBUG` payload
    entered, wrote `BCB command=boot-recovery`, chain-loaded patched ABL, installed
    hooks, and handed off. Recovery evidence (`recovery.props`, `cmdline`, dmesg)
    confirms `oplusboot.mode=recovery` / `ftmrecovery`.
  - Edge case: because the flashed sibling is old, it leaves logfs/debug-sink
    state open when entering FastbootLib. The staged payload logs
    `LogFs: ConnectController failed: Not Found` and falls back to console-only;
    therefore the new staged `UefiLog1.txt` snapshot path is built but not fully
    device-validated until a refreshed sibling with logfs close-before-Fastboot is
    flashed. The old sibling's still-installed console sink did mirror payload
    output into the sibling log file, explaining why payload logs appear in the
    older `GblSibling` log slot.
  - Pulled logfs contained no `UefiLogSavedN.txt` files, only `UefiLog1.txt`.
    `UefiLog1.txt` itself still shows the old sibling message
    `LogFs: staged context — skipping UefiLog rotation`, confirming this pull
    cannot validate the newly committed staged snapshot behavior until the
    refreshed sibling is flashed and can cleanly hand off logfs before FastbootLib.
- Refreshed sibling artifact prepared after committing the payload route/logfs
  changes: `./scripts/build.sh canoe RELEASE --mode auto-debug && ./tests/runall.sh`
  passed. Outputs: `dist/auto-debug.efi` and `dist/gbl-chainload.efi`, size
  `536576` bytes. This is the candidate to flash when ready to validate clean
  logfs close-before-Fastboot plus staged `UefiLog1.txt` snapshots.
- Manual log pull after the refreshed auto-debug artifact was flashed:
  `logs/20260508-081754_manual_auto-debug_v0.1-step1d/`.
  - Recovery state was confirmed by adb: `ro.bootmode=recovery`,
    `ro.boot.mode=recovery`, cmdline contains `oplusboot.mode=recovery` and
    `ftmrecovery`.
  - New UefiLog snapshot behavior is now visible: logfs contains
    `UefiLogSaved0.txt`, `UefiLogSaved1.txt`, `UefiLogSaved2.txt`, and
    `UefiLogSaved.idx` alongside `UefiLog1.txt`.
  - Auto-debug failure reason: stale/persistent `bootonce-bootloader` in BCB
    combines with boot reasons and causes unintended routing. Examples:
    `GblChainload_Boot1.txt`/`Boot2.txt` show
    `reason=0x3E bcb="bootonce-bootloader" recovery=0 bootloader=1` followed by
    `routing to OEM bootloader fastboot` and `bootloader: chain-loading patched
    ABL`. `UefiLogSaved1/2.txt` then show ABL `KeyPress:0, BootReason:62`, loads
    `boot_a`/normal Android images, and cmdline has `oplusboot.mode=reboot`
    rather than fastboot. In contrast, the final recovered boot had
    `reason=0x1 bcb="bootonce-bootloader" recovery=1 bootloader=1`, recovery won,
    and ABL loaded recovery successfully.
  - Conclusion: auto-debug is not fail-closed. `bootonce-bootloader` is not a
    reliable no-key software route to OEM bootloader through patched ABL; it can
    fall through toward normal Android boot. Continue primary validation with
    `mode-debug` and treat auto-debug as needing a later targeted fix/clear-Bcb
    policy before reuse as the default flashed sibling.
- Mode-debug artifact prepared as the safer flashed target after this finding:
  `./scripts/build.sh canoe DEBUG --mode mode-debug && ./tests/runall.sh` passed.
  Outputs: `dist/mode-debug.efi` and `dist/gbl-chainload.efi`, size `618496`
  bytes. Expected flashed behavior: no key writes BCB `boot-recovery` and enters
  recovery for adb log collection; user interrupt enters payload FastbootLib for
  raw logfs/data retrieval checks.
- Intermediate logs after flashing `dist/mode-debug.efi` and letting the no-key
  path boot recovery: `logs/20260508-082301_manual_mode-debug_v0.1-step1d/`.
  - adb confirms recovery (`ro.bootmode=recovery`, slot `_a`), and cmdline has
    `oplusboot.mode=recovery` / `ftmrecovery`.
  - `logfs/GblChainload_Boot2.txt` is the flashed mode-debug run (built
    `May  8 2026 14:19:10`). It shows the expected no-key path:
    `ModeDebug default recovery route: BCB command=boot-recovery`, then
    `chain-loading patched ABL`.
  - Hook coverage was captured before handoff: `ProtocolHookLib: finished -
    installed 4 of 4`, `BootFlow: finished - handing off to patched ABL`, and
    `EbsHook: ExitBootServices wrapper fired`.
  - `/proc/bootloader_log` corroborates the same run and shows ABL selected
    recovery (`Fastboot=0, Recovery:1`).
  - Logfs now contains the full `UefiLogSaved0..4.txt` ring plus `UefiLog1.txt`,
    so the staged/FV UefiLog snapshot/rotation path is producing durable context.
- User-interrupt FastbootLib test from flashed mode-debug:
  - Device entered FastbootLib and enumerated as fastboot serial `8549c105`.
  - `./scripts/pull-logfs.sh logs/20260508-082511_fastbootlib_mode-debug_logfs`
    reached our command and received the header
    `partition=logfs size=8388608 blkSize=4096 begin`, proving the command found
    the raw `logfs` BlockIo geometry.
  - The full INFO-stream transfer failed immediately afterward with
    `FAILED (Status read failed (No such device))`, producing a 0-byte
    `logfs.img`. The device re-enumerated/stayed in fastboot and simple queries
    still worked (`fastboot getvar product` -> `canoe`, `current-slot` -> `a`).
  - Interpretation: FastbootLib command dispatch works, but the huge
    ~8 MiB-as-30-byte-INFO-packets transfer is not viable as-is; it likely
    starves USB/event/watchdog handling long enough to drop/re-enumerate.
- Ghidra RE note for `LinuxLoader_infiniti.efi` watchdog behavior:
  - Connected to GUI project `gbl_root_canoe`, program `LinuxLoader_infiniti.efi`.
  - Renamed `FUN_00004e50` to `LinuxLoaderEntry` and annotated the fastboot
    watchdog block.
  - Evidence: strings `Fastboot: Initializing...` at `0x5ba56` and
    `Fastboot: Couldn't disable watchdog timer: %r` at `0x6ea7d` both xref from
    `LinuxLoaderEntry`. Around `0x6f28..0x6f40`, the function calls
    `gBS->SetWatchdogTimer(0, 0x10000, 0, NULL)` (BootServices function pointer
    at offset `0x100`) before continuing Fastboot initialization; failure logs
    the watchdog error string.
  - No evidence yet from this pass that LinuxLoader_infiniti performs the extra
    QcomWDog protocol `Disable`/pet-loop handling we added. It may simply avoid
    watchdog bites because stock fastboot commands are short and return to the
    event loop, unlike our long INFO-stream dump.
- Follow-up implementation: `oem get-staged logfs` was changed from a single
  full-partition INFO-stream into a host-driven chunked command:
  `oem get-staged logfs <offset> <length>`. The command now reads only the
  requested block-aligned range from raw BlockIo, caps each request at 64 KiB,
  and reports `offset`, `length`, total `size`, and `blkSize` in the INFO header.
  `scripts/pull-logfs.sh` now loops in small chunks (default 4096 bytes,
  override with `LOGFS_PULL_CHUNK_BYTES`) and reassembles `logfs.img`. This keeps
  each fastboot command short so Phoenix/USB/watchdog handling can return to the
  command/event loop between chunks.
- Host validation for chunked dump changes passed:
  `./scripts/build.sh canoe DEBUG --mode mode-debug && ./tests/runall.sh`, plus
  `bash -n scripts/pull-logfs.sh` and `git diff --check`.
- First device attempt with chunked dump failed at the first 4 KiB chunk because
  the INFO header was still too long for FastbootLib's response/USB packet size:
  `raw-chunks.txt` showed the header truncated at `blkSize=4`, followed by
  `FAILED (Status read failed (No such device))`. The wire format was shortened
  from verbose `partition=logfs offset=... length=... size=... blkSize=...` to
  compact `lfs o=<O> l=<L> s=<S> b=<B>` and `lfs-done o=<O> l=<L>`, and the
  host parser was updated. Rebuilt `dist/mode-debug.efi` and revalidated with
  `./scripts/build.sh canoe DEBUG --mode mode-debug && ./tests/runall.sh && bash -n scripts/pull-logfs.sh && git diff --check`.

### Boot shell / loader direction

- User performed the no-code watchdog hypothesis test successfully: boot through
  the bootloader-fastboot reset path first, then opt into our FastbootLib from
  the GBL key window. This stayed stable where the previous direct FastbootLib
  path hit watchdog timeout. This supports the theory that an earlier
  Oplus/Phoenix bootchain stage changes watchdog policy based on boot
  status/reason before LinuxLoader/FastbootLib runs.
- Future auto-debug should use two separate signals:
  1. bootchain signal: reset/reboot reason `FASTBOOT_MODE`, so early firmware
     identifies bootloader-fastboot and applies the stable watchdog policy
  2. GBL one-shot marker: tells `Entry.c` to consume this specific boot and enter
     our FastbootLib instead of letting patched ABL continue to stock fastboot
- Safety rules: recovery/VolUp always wins; if marker is present without
  `FASTBOOT_MODE`, clear it and fail closed; do not use `bootonce-bootloader` as
  this marker because it was shown to persist and fall through toward Android.
- Strategic direction: stop relying on fastboot INFO-stream file extraction as
  the main data path. Prefer staged EFI payloads, an ESP/USB/shell boot mode, and
  a small whitelisted OEM loader command surface for loading/starting staged EFI
  apps/drivers.

### Boot-shell / OEM EFI loader implementation validation

- Added `BOOT_SHELL_MODE` as a first-class build mode:
  `./scripts/build.sh canoe DEBUG --mode boot-shell` produces
  `dist/boot-shell.efi` and also updates `dist/gbl-chainload.efi`.
- `BOOT_SHELL_MODE` initializes normal GBL services, connects controllers via
  `SignalSDDetection()`/FV driver loading, scans mounted `SimpleFileSystem`
  volumes for `\EFI\BOOT\SHELLAA64.EFI`, `\EFI\BOOT\BOOTAA64.EFI`,
  `\SHELLAA64.EFI`, and `\SHELL.EFI`, then `LoadImage()`/`StartImage()`s the
  first candidate. If a candidate fails, scanning continues; if no candidate
  starts, it falls back to FastbootLib.
- Added debug-gated FastbootLib OEM EFI commands, registered only when one of the
  GBL debug modes is compiled (`AUTO_DEBUG_MODE`, `MODE_DEBUG`, or
  `BOOT_SHELL_MODE`):
  - `oem boot-efi` remains one-shot staged-image takeover.
  - `oem efi-load`, `oem efi-start`, `oem efi-unload`, `oem efi-status` provide a
    whitelisted load/start/return command surface.
  - `oem get-staged logfs <offset> <length>` remains a fallback raw BlockIo dump
    path, also debug-gated.
- Review fixes applied before validation:
  - Added `BOOT_SHELL_MODE` to `BootFlow.c`'s exact-one-mode guard.
  - Included `DevicePathLib` explicitly in `Entry.c` for `FileDevicePath()`.
  - Reduced logfs INFO data chunks to 28 bytes / 56 hex chars to fit Fastboot's
    64-byte response buffer with status prefix and NUL.
  - Added a fresh-stage latch so `oem boot-efi`/`oem efi-load` fail if no new
    `fastboot stage` completed, avoiding accidental load of stale initial buffer
    contents.
  - Preserve loaded EFI handles if `UnloadImage()` fails, so retry/status remains
    possible.
  - Made `scripts/pull-logfs.sh` source `device-monitor.sh` from the repo-root
    absolute path, so it is not dependent on the caller's cwd.
- Host validation passed after these fixes:
  `./scripts/build.sh canoe DEBUG --mode boot-shell && ./tests/runall.sh && ./scripts/build.sh canoe DEBUG --mode mode-debug && ./tests/runall.sh && bash -n scripts/build.sh scripts/build-inside-docker.sh scripts/pull-logfs.sh && git diff --check`.
  The last artifact is `dist/mode-debug.efi`/`dist/gbl-chainload.efi`, size
  `622592` bytes.
- Remaining device validation: boot-shell still needs staged/device testing with
  a real FAT/ESP containing a shell/boot AA64 EFI; OEM `efi-*` commands need a
  staged helper EFI test. USB host/mass-storage availability is still unproven on
  canoe and may require additional DXE drivers/platform host-controller support.

### FastbootLib-as-menu policy update

- Reframed FastbootLib/FastbootMenu as the boot menu and operator surface.
  `BOOT_SHELL_MODE` was removed from the advertised/buildable mode set because it
  depends on unproven external USB/ESP input and does not keep FastbootLib
  available while a non-returning shell/bootloader runs.
- Updated interrupt semantics:
  - `MODE_DEBUG`: VolUp enters our FastbootLib; VolDown is a logged placeholder;
    no key chain-loads patched ABL as-is without writing BCB or forcing recovery.
  - `AUTO_DEBUG_MODE`: default/no-key enters our FastbootLib; VolUp within a
    5-second window escapes by chain-loading patched ABL as-is; VolDown is a
    logged placeholder. This removes the previous BCB/reset-reason routing that
    could accidentally fall through toward normal Android with stale
    `bootonce-bootloader` state.
- Kept the debug-gated staged EFI command surface (`oem boot-efi`,
  `oem efi-load/start/unload/status`) as the near-term path for loading Shell.efi
  or smaller helper EFIs from FastbootLib. Full concurrent FastbootLib + Shell
  interaction is not assumed; practical requirement is that FastbootLib survives
  when the helper/shell returns.
- U-Boot fastboot research summary:
  - `oem run`, `UCmd`, and `ACmd` execute arbitrary bootloader commands and are
    explicitly debug/insecure patterns, disabled by default upstream.
  - Safer production-style pattern is fixed-purpose OEM handlers or a small
    compile-time vendor hook/whitelist, not arbitrary host-provided command
    strings.
  - For this tree, keep arbitrary command execution debug-only if added at all;
    prefer explicit commands for baked/staged shell/helper actions.
- Host validation passed after the policy update:
  `./scripts/build.sh canoe DEBUG --mode mode-debug && ./tests/runall.sh && ./scripts/build.sh canoe RELEASE --mode auto-debug && ./tests/runall.sh && bash -n scripts/build.sh scripts/build-inside-docker.sh scripts/pull-logfs.sh && git diff --check`.
  Final artifact after validation: `dist/auto-debug.efi` / `dist/gbl-chainload.efi`,
  size `540672` bytes. Build emitted a Docker/WSL clock-skew warning but completed
  and tests passed.
- Post-review fixes before final validation:
  - `CmdFlash()` now clears the fresh-stage latch immediately after consuming the
    fastboot download buffer, preventing `oem boot-efi` / `oem efi-load` from
    loading stale data after a `fastboot flash` operation.
  - `WaitForBootInterrupt()` no longer resets/flushed console input at the start
    of the key window, reducing the chance that an already-held VolUp escape is
    missed during `AUTO_DEBUG_MODE`'s 5-second window.
- Final host validation passed again with the same command sequence. Final
  artifact remains `dist/auto-debug.efi` / `dist/gbl-chainload.efi`, size
  `540672` bytes.

### Manual `fdt-cand[6]` recovery-log review

- User reported a visible/screen stall around `fdt-cand[6]` after entering the
  existing FastbootLib with VolUp, staging/booting a mode-debug payload, and
  rebooting into recovery. Logs were pulled with
  `./scripts/test-device-manual.sh mode-debug fdt-cand6-stall` into
  `logs/20260508-105532_manual_mode-debug_vfdt-cand6-stall/`.
- The pulled logs do contain `fdt-cand[6]`, but not as a selected FDT and not as
  a logged failure/stall:
  - `logfs/GblChainload_Boot3.txt:220-255`: EBS wrapper fired, candidates were
    scanned, `fdt-cand[6]=BF6EA124 size=563 chosen=-1`, and final selection was
    `fdt-selected=BF4CA7018`.
  - `logfs/GblChainload_Boot1.txt:220-256`: same pattern with
    `fdt-cand[6]=BF6E1124 size=563 chosen=-1`, final selection
    `fdt-selected=BF4CA7018`.
  - Both logs continue through bootargs/stdout-path/bootconfig inspection after
    candidate 6; there is no durable log evidence that candidate 6 itself hung.
- Recovery state corroborates successful Linux/recovery boot rather than a hard
  EBS/FDT stall: `getprop.boot.txt` shows `ro.boot.mode=recovery`,
  `ro.bootmode=recovery`, `ro.boot.dtb_idx=8`, `ro.boot.dtbo_idx=6`, slot `_a`,
  and orange/unlocked vbmeta state.
- Artifact-policy mismatch: the latest pull shows old mode-debug strings and
  behavior (`Press VolDown (fastboot) or VolUp (recovery) within 3s...`,
  `ModeDebug default recovery route: BCB command=boot-recovery`) from build
  `May  8 2026 15:23:10`. Current committed `Entry.c` at root `b4a4ee5` instead
  prints `Press VolUp for FastbootLib ... no key boots patched ABL as-is` and no
  longer writes BCB/forces recovery on the no-key mode-debug path.
- Conclusion: this pull is useful evidence for the old recovery-forcing
  mode-debug path, but it does not validate the current committed mode-debug
  policy and does not capture a real `fdt-cand[6]` failure. Next device run
  should stage a freshly rebuilt `dist/mode-debug.efi` from root `b4a4ee5` with
  edk2 `676e4887a7` and verify the new prompt appears before interpreting FDT
  behavior.
- Fresh current mode-debug artifact was rebuilt and host-validated after this
  review: `./scripts/build.sh canoe DEBUG --mode mode-debug && ./tests/runall.sh
  && bash -n scripts/build.sh scripts/build-inside-docker.sh scripts/pull-logfs.sh
  scripts/device-monitor.sh scripts/test-device.sh scripts/test-device-automatic.sh
  scripts/test-device-manual.sh && git diff --check` passed. Output artifacts:
  `dist/mode-debug.efi` and `dist/gbl-chainload.efi`, size `622592` bytes. UTF-16
  string check confirms the current prompt/policy is embedded:
  `Press VolUp for FastbootLib ... no key boots patched ABL as-is`,
  `ModeDebug default route`.
- Retried manual log pull after the fresh build with
  `./scripts/test-device-manual.sh mode-debug current-policy-fdt-review-retry`.
  Logs were captured in
  `logs/20260508-110238_manual_mode-debug_vcurrent-policy-fdt-review-retry/`.
  This pull still shows the old flashed/recovery-forcing artifact, not the fresh
  current staged artifact:
  - `logfs/GblChainload_Boot4.txt:1-8` and `Boot1.txt:1-8` show build time
    `May  8 2026 15:23:10`, prompt `Press VolDown (fastboot) or VolUp
    (recovery) within 3s...`, and `ModeDebug default recovery route: BCB
    command=boot-recovery`.
  - No current-policy strings (`Press VolUp for FastbootLib`, `VolDown
    placeholder`, `ModeDebug default route`) were found in the fresh pull.
  - `GblChainload_Boot4.txt:220-267` again shows EBS scanning completed:
    `fdt-cand[6]=BF6DD124 size=563 chosen=-1`, selected FDT
    `BF4CA7018`, bootargs chunks, stdout-path, and bootconfig scan. No durable
    evidence of a `fdt-cand[6]` stall.
  - Runtime state still confirms recovery: `getprop.boot.txt` has
    `ro.boot.mode=recovery`, `ro.bootmode=recovery`, `dtb_idx=8`, `dtbo_idx=6`,
    and slot `_a`; `cmdline`/`dmesg` contain `oplusboot.mode=recovery` and
    `ftmrecovery`.
- Updated conclusion: both manual pulls are recovery logs from the old flashed
  mode-debug artifact. To validate the new policy/FDT behavior, the device must
  first execute the freshly rebuilt `dist/mode-debug.efi` via FastbootLib
  `fastboot stage dist/mode-debug.efi` + `fastboot oem boot-efi`, and the screen
  or logs must show the new `Press VolUp for FastbootLib` prompt.
- Follow-up audit/correction: pulled logs can legitimately mix outer-loader and
  staged-payload context. `LogFsInit()` snapshots `UefiLog1.txt` in staged context
  without deleting it (`LogFsRotateUefiLog(..., FALSE)`), while `PostGblLog.c`
  clobbers the selected `GblChainload_BootN.txt` slot and writes a fresh banner.
  Therefore `UefiLogSavedN.txt` can preserve prior bootchain context, but a
  `GblChainload_BootN.txt` banner and prompt should identify the build that wrote
  that post-GBL slot.
- The retry logs do show a staged payload executed: `UefiLog1.txt:780-789` has
  `download process is complete`, `Loading GBL app`, `Starting GBL app`, then a
  MODE_DEBUG banner with build `May  8 2026 15:23:13`, followed by
  `LogFs: staged context ... snapshotting UefiLog1.txt` and `LogFs post-GBL:
  opened \GblChainload_Boot4.txt`.
- However, that staged payload still does not match the rebuilt current artifact:
  local `dist/mode-debug.efi` contains build strings `May  8 2026 16:58:31..35`
  and the current UTF-16 prompt `Press VolUp for FastbootLib ... no key boots
  patched ABL as-is`; the device log contains build `15:23:13` and old prompt
  `Press VolDown (fastboot) or VolUp (recovery) within 3s...`. Revised
  confidence: high that logging is preserving mixed context by design, and high
  that the executed staged payload in this pull was not the freshly rebuilt
  16:58 artifact.
- Banner identity update: console and post-GBL log banners now use a concise
  `gbl-chainload - <MODE> - <DATE> <TIME>` format, and the Fastboot menu displays
  the same identity as three separate lines: `gbl-chainload`, `MODE - <MODE>`,
  and `BUILD - <DATE> <TIME>`. This should make future screen/log captures easier
  to correlate with the exact staged artifact. Host validation passed after the
  change with `./scripts/build.sh canoe DEBUG --mode mode-debug &&
  ./tests/runall.sh && bash -n scripts/build.sh scripts/build-inside-docker.sh
  scripts/pull-logfs.sh scripts/device-monitor.sh scripts/test-device.sh
  scripts/test-device-automatic.sh scripts/test-device-manual.sh && git diff
  --check`. Output artifacts: `dist/mode-debug.efi` and `dist/gbl-chainload.efi`,
  size `622592`, built around `May  8 2026 17:10`.

### EBS/yellow-screen regression investigation branch

- Created root and edk2 branch `debug-ebs-regression` for staged-only fault
  isolation; no commits were created.
- Added build option `--debug-variant` / `GBL_DEBUG_VARIANT` with variants:
  `full`, `patch-only`, `no-ebs`, `ebs-wrapper-only`, `ebs-no-bootconfig`, and
  `ebs-no-close`. Variant builds write distinct artifacts such as
  `dist/mode-debug-patch-only.efi` while also updating `dist/gbl-chainload.efi`.
- Added forced phase breadcrumbs gated by debug variants:
  - `DBGPHASE: BootFlow: ...` around slot resolve, ABL unwrap, patching,
    protocol-hook install, `LoadImage`, and `StartImage`.
  - `DBGPHASE: EBS: ...` around EBS wrapper fire, slot restore, memory-map
    retrieval, FDT scan, `LogFdtChosen`, logfs close/remove, and original EBS
    tailcall.
  - Debug variants set `GBL_DEBUG_PHASE_FLUSH=1` so these breadcrumbs call
    `LogFsFlush()` immediately instead of relying on the 4 KiB dirty threshold.
- Isolation behavior:
  - `patch-only`: skips all protocol hooks after ABL patching.
  - `no-ebs`: installs QSEE/SCM/VerifiedBoot hooks but skips the EBS hook.
  - `ebs-wrapper-only`: EBS wrapper restores the slot and immediately tail-calls
    original EBS, skipping FDT scan/logfs close work.
  - `ebs-no-bootconfig`: runs FDT scan but skips the large bootconfig dump.
  - `ebs-no-close`: runs full FDT/bootconfig logic but flushes only and skips
    `LogFsRemoveDebugSink()`/`LogFsClose()` before original EBS.
- Host validation: all variants built successfully, scripts passed `bash -n`,
  `./tests/runall.sh` passed, and both root and edk2 `git diff --check` passed.
  Final artifacts present:
  - `dist/mode-debug-patch-only.efi` (`598016` bytes)
  - `dist/mode-debug-no-ebs.efi` (`622592` bytes)
  - `dist/mode-debug-ebs-wrapper-only.efi` (`618496` bytes)
  - `dist/mode-debug-ebs-no-bootconfig.efi` (`622592` bytes)
  - `dist/mode-debug-ebs-no-close.efi` (`622592` bytes)
  - `dist/mode-debug.efi` (`622592` bytes, full/current)
- Recommended device order: stage/test `mode-debug-patch-only.efi` first. If it
  still yellow-crashes, focus on ABL patch/current no-key handoff. If it boots,
  test `mode-debug-no-ebs.efi`; if that boots, EBS is implicated. Then test
  `ebs-wrapper-only`, `ebs-no-bootconfig`, and `ebs-no-close` to localize within
  the EBS body/lifecycle.
- Device result update: user reported test 1 (`mode-debug-patch-only.efi`) and
  test 2 (`mode-debug-no-ebs.efi`) both succeeded and device was in recovery.
  Pulled logs with
  `./scripts/test-device-manual.sh mode-debug ebs-regression-patch-only-no-ebs-success`
  into `logs/20260508-113317_manual_mode-debug_vebs-regression-patch-only-no-ebs-success/`.
  Evidence:
  - `logfs/UefiLogSaved3.txt:1046-1083` is the patch-only run, build
    `May  8 2026 17:22:02`, with `BootFlow: debug patch-only variant - protocol
    hooks skipped`, then `DBGPHASE: BootFlow: before StartImage`; ABL continues
    to recovery (`Fastboot=0, Recovery:1` at line 1112).
  - `logfs/UefiLog1.txt:886-929` is the no-EBS run, build
    `May  8 2026 17:22:07`, with QSEE/SCM/VerifiedBoot hooks installed and
    `ProtocolHook: ebs skipped by no-ebs debug variant`, then handoff before
    StartImage; recovery boot continues.
  - Older ring slots still contain prior full/EBS-hook runs, e.g.
    `GblChainload_Boot2.txt` shows old build `15:23:10` and EBS scan through
    `fdt-selected=BF4CA7018`; do not confuse these with the current two success
    runs.
  Interim conclusion: ABL unwrap/patch, no-key handoff, and non-EBS protocol
  hooks are not sufficient to trigger the yellow-line crash. Next isolating test
  should be `mode-debug-ebs-wrapper-only.efi`.
- Device result update 2: user reported test 3a (`mode-debug-ebs-wrapper-only.efi`)
  succeeded, while 3b (`mode-debug-ebs-no-bootconfig.efi`) failed. User observed
  the screen stuck for a long time around `fdt-cand[9]`, progressing to
  `bc-cand[7]`, then crashing. Pulled recovery logs into
  `logs/20260508-114808_manual_mode-debug_vebs-wrapper-success-no-bootconfig-bc-cand7-crash/`.
  Evidence:
  - Current wrapper/no-bootconfig attempt identity appears in `UefiLogSaved*.txt`
    as build `May  8 2026 17:22:12`, with EBS hook installed and BootFlow reaching
    `DBGPHASE: BootFlow: before StartImage` (`UefiLogSaved4.txt:886-929`).
  - The current staged run again failed to mount logfs in staged context
    (`ConnectController failed: Not Found`), so EBS-phase breadcrumbs from the
    failed run may not be durable in `GblChainload_BootN.txt`; on-screen evidence
    is important for this case.
  - `GblChainload_Boot0.txt:220-268` contains an older full/EBS scan ring slot
    (old build `15:23:10`) that completed through `mem-scan` and final
    `bootconfig: no sane #BOOTCONFIG` line; do not treat that as proof the
    current no-bootconfig variant completed.
  - Recovery userspace provides the data we were trying to discover by risky EBS
    memory scanning: pulled `/proc/cmdline`, `/proc/bootconfig`, and
    `getprop.boot.txt`. `/proc/bootconfig` contains canonical Android bootconfig
    key/value pairs including `androidboot.mode`, `dtbo_idx`, `dtb_idx`, vbmeta
    digests, slot suffix, etc.; `cmdline` contains the kernel command line and
    `oplusboot.mode=recovery`.
  Revised conclusion: `ebs-wrapper-only` success plus `no-bootconfig` failure
  strongly implicates the FDT/bootconfig memory scan/logging body, not the EBS
  table hook itself and not the final bootconfig blob dump. The current scan is
  too invasive for the boot path and should be replaced or made strictly offline/
  diagnostic.

### Source-driven boot argument plan

- Source mapping of the CLO LA/QcomModulePkg boot path found explicit, safe
  serialization points for both `/chosen/bootargs` and bootconfig:
  - `LinuxLoader.c` calls `BootLinux(&Info)` after image/auth setup.
  - `BootLinux.c` calls `UpdateCmdLine(...)`, then appends bootconfig via
    `AddBootconfigParameters()`, then calls `UpdateDeviceTree(...)`.
  - `UpdateCmdLine.c` builds `BootParamlist.FinalCmdLine` and
    `BootParamlist.FinalBootConfig`.
  - `UpdateDeviceTree.c` writes `/chosen/bootargs` from `FinalCmdLine`.
  - `Bootconfig.c` appends the Android bootconfig v4 payload/trailer.
- Runtime policy changed on `debug-ebs-regression`: full/default MODE_DEBUG now
  keeps the EBS hook wrapper-only (`GBL_DEBUG_EBS_SCAN=0`). The unsafe memory
  scan remains only in explicit diagnostic variants (`ebs-scan`,
  `ebs-no-bootconfig`, `ebs-no-close`).
- Added `docs/re/qcom-boot-arg-mutation.md` with the source-based strategy and
  safe mutation points. Added device-tree snapshotting to
  `scripts/test-device-manual.sh` (`device-tree.tar`) so recovery pulls capture
  `/proc/device-tree` or `/sys/firmware/devicetree/base` alongside
  `/proc/cmdline`, `/proc/bootconfig`, and `getprop.boot.txt`.

### Safe EBS default validation

- Device validation capture:
  `logs/20260508-115859_manual_mode-debug_vsafe-ebs-wrapper-default/`.
- Recovery userspace confirms successful boot after staged `mode-debug` built
  `May 8 2026 17:57:14` with `GBL_DEBUG_EBS_SCAN=0`:
  - `recovery.props`: bootmode `recovery`, slot `_a`.
  - `getprop.boot.txt`: `ro.boot.mode=recovery`, `ro.boot.slot_suffix=_a`,
    `ro.boot.dtb_idx=8`, `ro.boot.dtbo_idx=6`, `ro.boot.verifiedbootstate=orange`.
  - `bootconfig`: 61 Android bootconfig entries including
    `androidboot.mode = "recovery"`, `androidboot.dtb_idx = "8"`,
    `androidboot.dtbo_idx = "6"`, `androidboot.slot_suffix = "_a"`.
  - `cmdline`: contains `bootconfig`, `oplus_ftm_mode.oplus_ftm_mode=ftmrecovery`,
    `oplusboot.mode=recovery`.
  - `device-tree.tar` was captured successfully.
- Current-build evidence is in `logfs/UefiLog1.txt:886-929`: new prompt, no-key
  route to patched ABL, protocol hooks installed, EBS wrapper installed, and
  handoff reached `StartImage`. No current-build `fdt-cand`/`bc-cand` scan lines
  appear. `GblChainload_Boot*.txt` still contains stale older ring entries, so
  use build timestamp/banner to identify the actual run.

### Stock ABL EBS probing strategy

- Correction: because this project chainloads stock ABL, source edits to this
  checkout's BootLib do not observe stock ABL unless we rebuild/replace ABL.
  For stock-chainload runs, valid observation points are pre-`StartImage` hooks,
  the `gBS->ExitBootServices` wrapper, or deliberate binary patching of stock
  ABL.
- Added explicit diagnostic variant `--debug-variant ebs-fdt-probe`:
  - default/full `mode-debug` remains wrapper-only with `GBL_DEBUG_EBS_SCAN=0`
    and `GBL_DEBUG_EBS_FDT_PROBE=0`;
  - diagnostic variant sets `GBL_DEBUG_EBS_FDT_PROBE=1` and leaves the old
    unsafe `GBL_DEBUG_EBS_SCAN=0`.
- Diagnostic behavior is log-only and fail-closed:
  - restore `ExitBootServices` slot before any probe work;
  - fetch memory map once;
  - scan only `EfiBootServicesData`/`EfiLoaderData` for validated FDT headers;
  - require sane FDT total size and `/chosen`;
  - log only compact facts: FDT address/size, bootargs prefix, initrd start/end;
  - parse `/chosen/linux,initrd-start` and `/chosen/linux,initrd-end`;
  - search only the last 128 KiB of that bounded initrd range for
    `#BOOTCONFIG\n` trailer;
  - no mutation and no blob dumps.
- Host validation:
  - `mode-debug` default rebuilt successfully (`May 8 2026 18:06:00`,
    `dist/mode-debug.efi` and `dist/gbl-chainload.efi`, 557056 bytes).
  - `mode-debug --debug-variant ebs-fdt-probe` built successfully
    (`dist/mode-debug-ebs-fdt-probe.efi`, 557056 bytes).
  - `bash -n`, `git diff --check`, `git -C edk2 diff --check`, and
    `./tests/runall.sh` passed. Final `dist/gbl-chainload.efi` restored to the
    default wrapper-only build, not the diagnostic variant.

### `ebs-fdt-probe` device result

- Device validation capture:
  `logs/20260508-120731_manual_mode-debug_vebs-fdt-probe/`.
- Diagnostic payload booted successfully to recovery:
  - `recovery.props`: bootmode `recovery`, slot `_a`.
  - `getprop.boot.txt`: `ro.boot.mode=recovery`, `ro.boot.slot_suffix=_a`,
    `ro.boot.dtb_idx=8`, `ro.boot.dtbo_idx=6`.
  - `bootconfig`: 61 entries, including `androidboot.mode = "recovery"`,
    `androidboot.dtb_idx = "8"`, `androidboot.dtbo_idx = "6"`,
    `androidboot.slot_suffix = "_a"`.
  - `cmdline`: contains `bootconfig`, `oplusboot.mode=recovery`, and
    `oplus_ftm_mode.oplus_ftm_mode=ftmrecovery`.
- Current GBL evidence is in `logfs/UefiLog1.txt:886-929`:
  - banner `gbl-chainload - MODE_DEBUG - May 8 2026 18:05:42`;
  - `LogFs: ConnectController failed: Not Found`, so persistent GBL logfs sink
    was unavailable for the current run;
  - EBS wrapper installed and handoff reached `StartImage`.
- No current-run `ebs-fdt-probe` lines were persisted. The apparent
  `fdt-cand`/`bc-cand` lines in `logfs/GblChainload_Boot0.txt:220-268` are stale
  from the old full-scan build; they use the old `ebs-call | fdt-cand[...]`
  format rather than the new `ebs-fdt-probe | ...` format and have the old boot
  slot ring behavior.
- Conclusion: the bounded diagnostic path appears boot-safe, but the capture
  channel is insufficient on staged runs when `LogFsLib` cannot mount. Next
  instrumentation step should add a persistent low-volume fallback for current
  EBS facts or repair LogFs availability before relying on EBS diagnostics.

### AUTO_DEBUG_MODE readiness check

- Readiness reviewed before user test/flash. AUTO_DEBUG_MODE policy remains:
  - no key -> FastbootLib;
  - VolDown -> placeholder, then FastbootLib;
  - VolUp within 5s -> patched ABL escape, no BCB write/clear.
- AUTO_DEBUG_MODE chainload path skips protocol hooks (`BootFlow.c` compiles
  hooks only for `MODE_DEBUG`/`MODE_1`), so it does not install QSEE/SCM/VB/EBS
  hooks and cannot run the unsafe EBS scan/probe paths.
- Built the correct AUTO_DEBUG artifact with `./scripts/build.sh --mode
  auto-debug` after catching that positional `auto-debug` is parsed as the board
  name, not the mode selector.
- Current flash/test artifact is now `dist/gbl-chainload.efi` ==
  `dist/auto-debug.efi`, built May 8 2026 18:12, size 540672 bytes.
- Verified embedded mode strings include `AUTO_DEBUG_MODE` and
  `MODE - AUTO_DEBUG_MODE`.
- Host checks passed after the AUTO_DEBUG build: `./tests/runall.sh` plus earlier
  `bash -n`, `git diff --check`, and `git -C edk2 diff --check`.
- Caveat: build output included a Docker/BaseTools `Clock skew detected` warning,
  but the build is clean-from-scratch and tests passed. If timestamps look odd on
  device, rebuild once more before flashing.

### BCB-free recovery escape automation implementation

- Implemented the next host/device-flow step after AUTO_DEBUG behavior was
  confirmed correct by the user.
- FastbootLib changes in `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c`:
  - added `oem escape`, guarded by `GBL_EXPERIMENTAL_FASTBOOT_CMDS`, which calls
    `BootFlowChainLoad()` to leave AUTO_DEBUG FastbootLib and start the patched
    stock ABL path;
  - changed `reboot recovery` handling to use `RebootDevice(RECOVERY_MODE)`
    instead of writing `RECOVERY_BOOT_RECOVERY` into BCB and rebooting normally.
    This keeps the recovery request as reset-reason state for the next boot and
    avoids persistent BCB routing for the automation path.
- Host-script changes:
  - `scripts/device-monitor.sh` now has `device_monitor_wait_for_fastboot()` for
    polling FastbootLib re-enumeration through the shared fastboot lock.
  - `scripts/test-device-automatic.sh --escape-recovery` now performs:
    `fastboot reboot recovery` -> wait for AUTO_DEBUG FastbootLib -> sleep
    `${GBL_TEST_ESCAPE_DELAY:-5}` -> `fastboot oem escape` -> wait for adb
    recovery/device -> pull logs.
  - Legacy `--bcb=*` arguments now fail with guidance to use `--escape-recovery`.
  - Fastboot command transcripts are stored in the run log directory as
    `fastboot-oem-escape.txt` or `fastboot-oem-boot-efi.txt` instead of `/tmp`.
- Host validation passed after these changes:
  - `bash -n scripts/device-monitor.sh scripts/test-device-automatic.sh
    scripts/test-device-manual.sh scripts/test-device.sh scripts/pull-logfs.sh
    scripts/build.sh scripts/build-inside-docker.sh`
  - `git diff --check`
  - `git -C edk2 diff --check`
  - `./tests/runall.sh`
  - `./scripts/build.sh --mode auto-debug`
  - `strings dist/gbl-chainload.efi | grep -E
    'AUTO_DEBUG_MODE|MODE - AUTO_DEBUG_MODE|oem escape|escaping to patched ABL'`
- Current final artifact after validation: `dist/gbl-chainload.efi` ==
  `dist/auto-debug.efi`, size 540672 bytes, includes `oem escape` and
  AUTO_DEBUG mode/menu strings.
- Device validation still pending and should be user-controlled:
  `./scripts/test-device-automatic.sh --escape-recovery` with the refreshed
  AUTO_DEBUG artifact flashed as sibling.

### `--escape-recovery` device validation

- Before running the automation, the device was visible as AUTO_DEBUG FastbootLib
  serial `8549c105`. To satisfy the watchdog preconditioning requirement, issued
  a bootloader reboot path first and waited for FastbootLib to reappear:
  `fastboot reboot bootloader` -> `device_monitor_wait_for_fastboot 120`.
- Ran `./scripts/test-device-automatic.sh --escape-recovery` successfully.
  Capture directory:
  `logs/20260508-123105_auto_escape-recovery_vb4a4ee5/`.
- Host-side flow evidence:
  - `fastboot-oem-escape.txt` contains `(bootloader) escaping to patched ABL`
    followed by `FAILED (Status read failed (No such device))`, which is the
    expected USB drop on `StartImage` handoff.
  - adb came up as `state=recovery`.
  - script pulled `/proc/bootloader_log`, `/proc/bootconfig`, `/proc/cmdline`,
    `dmesg.txt`, boot props, and 13 logfs files, then returned the device to
    sibling FastbootLib.
  - post-run check: `fastboot devices -l` shows `8549c105 fastboot`, and
    `fastboot getvar product` reports `canoe`.
- Recovery userspace evidence:
  - `recovery.props`: `ro.bootmode=recovery`, slot `_a`.
  - `bootconfig`: `androidboot.mode = "recovery"`, `androidboot.dtb_idx = "8"`,
    `androidboot.dtbo_idx = "6"`, `androidboot.slot_suffix = "_a"`.
  - `cmdline`/`dmesg.txt`: contain `oplusboot.mode=recovery` and
    `oplus_ftm_mode.oplus_ftm_mode=ftmrecovery`.
- Bootloader/logfs evidence in `logfs/UefiLog1.txt`:
  - current AUTO_DEBUG build banner: `May 8 2026 18:25:40`.
  - AUTO_DEBUG entered default/no-key FastbootLib, installed watchdog defenses
    (`SetWatchdogTimer` hook, `QcomWDog->Disable()`, `SetBiteTimeout(0)`, 5s pet).
  - command log shows `[21289]Handling Cmd: oem escape`.
  - `BootFlow` loaded patched ABL from `abl_a`, applied EFISP recursion and orange
    screen patches, skipped protocol hooks for AUTO_DEBUG, and handed off via
    `StartImage`.
  - stock ABL observed `BootReason:1`, `Fastboot=0, Recovery:1`, loaded recovery
    images, and logged `Booting into Recovery Mode via Boot`.
- Conclusion: the BCB-free recovery automation is device-validated. The stable
  sequence is: precondition through bootloader reboot path for watchdog policy,
  `fastboot reboot recovery`, wait for AUTO_DEBUG FastbootLib, `oem escape`, then
  collect recovery logs over adb and return to sibling FastbootLib.

### T1 closeout additions: EBS-only probe and RPMB operator hooks

- Added dangerous/debug-only FastbootLib commands under
  `GBL_EXPERIMENTAL_FASTBOOT_CMDS`:
  - `fastboot oem rpmb-lock CONFIRM_WIPE`
  - `fastboot oem rpmb-unlock CONFIRM_WIPE`
- These commands intentionally call `SetDeviceUnlockValue(UNLOCK, state)`
  directly. They fail without the confirmation token and warn that ABL is not
  patched for this and that the primitive requests data wipe via misc. They were
  built and string-verified but not executed.
- Added `--debug-variant ebs-fdt-probe-only`, which sets
  `GBL_DEBUG_EBS_FDT_PROBE=1` and `GBL_DEBUG_EBS_ONLY=1`, causing
  `InstallAll.c` to skip QSEE/SCM/VerifiedBoot hooks and install only EBS. This
  was needed because the full hook probe booted, but current EBS facts were not
  visible in the 64 KiB bootloader-log window after QSEE/SCM/VB log traffic.
- Host validation passed:
  - `bash -n` for build/device scripts
  - `git diff --check`
  - `git -C edk2 diff --check`
  - `./scripts/build.sh --mode mode-debug --debug-variant ebs-fdt-probe`
  - `./scripts/build.sh --mode mode-debug --debug-variant ebs-fdt-probe-only`
  - `./scripts/build.sh --mode auto-debug`
  - `./tests/runall.sh`
  - strings verified for AUTO_DEBUG, `oem escape`, RPMB commands, and
    `CONFIRM_WIPE`.
- Device validation captures:
  - full hook EBS probe:
    `logs/20260508-124328_auto_mode-debug-ebs-fdt-probe_vb4a4ee5/`
  - EBS-only probe:
    `logs/20260508-124706_auto_mode-debug-ebs-fdt-probe-only_vb4a4ee5/`
- Full hook probe booted to recovery and showed representative hook coverage in
  `UefiLog1.txt`: QSEE Keymaster `SET_ROT` and `SET_BOOT_STATE`, SCM secure-state
  reads, VB RWDeviceState reads/writes, and all hooks installed. EBS facts were
  not current-run visible due to log-window pressure.
- EBS-only probe booted to recovery and confirmed current build installed only
  EBS (`qseecom/scm/verifiedboot skipped`, `installed 1 of 1`). Persisted probe
  facts still show bounded FDT candidates but no selected `/chosen` tree
  (`summary ... fdt-hits=12 selected=0`), supporting the conclusion that EBS is
  useful as a lifecycle/probe hook, not the primary bootarg mutation anchor.
- Wrote T1 closeout report:
  `docs/re/t1-closeout-report.md`.
- Final artifact restored to AUTO_DEBUG after diagnostic runs:
  `dist/gbl-chainload.efi == dist/auto-debug.efi`, size 540672 bytes, includes
  `oem escape`, `oem rpmb-lock`, `oem rpmb-unlock`, and `CONFIRM_WIPE` strings.
