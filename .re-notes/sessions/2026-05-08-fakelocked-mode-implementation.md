# 2026-05-08 — mode-fakelocked implementation checkpoint

## Branching / base

- Root checkpoint committed on `debug-ebs-regression`: `20bef55 Checkpoint EBS and UDT debug work`.
- edk2 checkpoint committed on `debug-ebs-regression`: `2c4c2d629c FastbootLib: add recovery escape controls`.
- Dead-end preservation branches created:
  - root: `dead-end/ebs-udt-debug`
  - edk2: `dead-end/ebs-udt-debug`
- Active implementation branch: `mode-fakelocked`.
- Chosen base: the checkpoint above, because it already contains the mode framework, staged EFI/oem escape flow, hook installer, and host patch harnesses. The unsafe EBS/UDT helper work remains gated as debug-only and preserved on the dead-end branch.

## Implemented source behavior

- Added first-class `mode-template` as the reusable interactive chainload base.
- Added first-class `mode-fakelocked` deriving from the same template flow:
  - no key: enter FastbootLib and await `oem escape`;
  - VolUp: immediately chain-load patched ABL;
  - VolDown: placeholder/default FastbootLib route.
- Extended BootFlow hook gate so `MODE_TEMPLATE` and `FAKELOCKED` install protocol hooks.
- Extended build scripts and DSC defines for `mode-template` and `mode-fakelocked`.
- Extended FastbootLib experimental staged commands for `MODE_TEMPLATE` and `FAKELOCKED`.
- Extended mode identity strings for post-GBL log and Fastboot menu.
- After code review, `FAKELOCKED` no longer uses the full best-effort debug hook installer. BootFlow calls `ProtocolHook_InstallFakelockedRequired()` and aborts chain-load if the VerifiedBoot hook set cannot be installed.

## VerifiedBoot protocol fakelock policy

In `FAKELOCKED` builds only, `VerifiedBootHook.c` now mutates the `QCOM_VERIFIEDBOOT_PROTOCOL` view:

- `VBRwDeviceState(READ_CONFIG, DeviceInfo*)`:
  - calls original;
  - if successful, overwrites `DeviceInfo.is_unlocked = FALSE` and `DeviceInfo.is_unlock_critical = FALSE` in the returned buffer.
- `VBRwDeviceState(WRITE_CONFIG, ...)`:
  - does not call original;
  - returns `EFI_SUCCESS` and logs `fakelock=WRITE swallowed`.
- `VBDeviceInit(device_info_vb_t*)`:
  - clears `is_unlocked` and `is_unlock_critical` before calling original, so the protocol/TZ path sees locked;
  - clears them again after return so ABL's caller-visible copy remains locked.
- `VBDeviceResetState()`:
  - does not call original;
  - returns `EFI_SUCCESS` and logs `fakelock=reset swallowed`.
- All other VerifiedBoot slots remain pass-through/logging.
- Critical policy is enforced on reentrant calls too. The reentry guard suppresses recursive logging only; it no longer bypasses READ/WRITE/reset fakelock semantics.
- In `FAKELOCKED` builds, `InstallVerifiedBootHook()` fails closed if any critical slots are missing: `VBRwDeviceState`, `VBDeviceInit`, or `VBDeviceResetState`.

## Remaining edge work

- This source pass does not yet add the optional ABL `patch11-avb-result-clamp` for `OK_NOT_SIGNED`/missing-vbmeta edge cases. That still needs a Ghidra-anchored branch rewrite before device validation against unsigned recovery images.
- Do not run old UDT helper payloads by default; previous routed helper test caused device power-off/hang.

## Tests added

- `tests/045_fakelocked_mode_lint.sh` checks source/build wiring and the key fakelock hook behavior markers.

## Device validation notes

- `dist/mode-fakelocked.efi` was rebuilt and contains the expected fakelocked identity/policy strings:
  - ASCII `FAKELOCKED`, `FakeLocked`, `fakelock`, `gbl-chainload` present.
  - UTF-16LE `ProtocolHook_InstallFakelockedRequired` present.
  - `AUTO_DEBUG` / `MODE_DEBUG` strings absent from the EFI image.
- Failed automatic run: `logs/20260508-183221_auto_mode-fakelocked_vf91f6f3` stopped at `fastboot stage ...` with remote `unknown command`.
- Manual workaround run: `logs/20260508-183515_manual-fakelocked-stage-workaround` used `fastboot boot dist/mode-fakelocked.efi`, which generated/sent an Android `boot.img` wrapper. Treat this as invalid evidence for direct EFI staging.
- Recovery capture after the workaround: `logs/20260508-183729_manual_mode-fakelocked_vf91f6f3` remained orange/unlocked and showed the permanent `AUTO_DEBUG_MODE` path:
  - `gbl-chainload - AUTO_DEBUG_MODE - May  8 2026 18:25:40`
  - `BootFlow: before ProtocolHook_InstallAll`
  - no `ProtocolHook_InstallFakelockedRequired` / `vb-fakelock` markers.
  - KeyMaster log still had `isUnlocked=1`, color orange.
- A later direct recheck from a fresh GBL FastbootLib session showed `fastboot stage dist/mode-fakelocked.efi` can succeed (`OKAY`, 532 KiB). Therefore the main open item is a clean stage + `oem boot-efi` + `oem escape` validation, not yet a confirmed source defect.
- Current device/USB state after attempting to re-enter fastboot is unstable: `adb devices` lists the phone as `device`, but `adb shell`/`exec-out` close immediately and `adb reboot bootloader` did not yield fastboot within 180s. Needs manual/device-side recovery before the clean staged validation can continue.
- Clean automatic staged run: `logs/20260508-194854_auto_mode-fakelocked_vf91f6f3`.
  - `fastboot stage dist/mode-fakelocked.efi` succeeded.
  - `oem boot-efi` started the staged EFI (`loading staged 544768 bytes`, USB drop expected).
  - `FAKELOCKED` payload executed and entered FastbootLib.
  - `oem escape` invoked fakelocked `BootFlowChainLoad`.
  - `ProtocolHook_InstallFakelockedRequired` ran successfully; `VerifiedBootHook: installed 10 of 10 slots`.
  - Hook evidence present: repeated `vb-fakelock | READ_CONFIG | is_unlocked 1->0 | is_unlock_critical 1->0` and `fakelock=WRITE swallowed`.
  - Despite hook success, AVB still produced orange/unlocked in captured recovery evidence: `androidboot.verifiedbootstate = "orange"`, `androidboot.vbmeta.device_state = "unlocked"`, `ro.boot.verifiedbootstate=orange`, `ro.boot.vbmeta.device_state=unlocked`.
  - Logs also show `avb_slot_verify.c:872 ... OK_NOT_SIGNED` and `AvbSlotVerify returned ERROR_VERIFICATION`, so the next technical target is direct AVB/`IsUnlocked()`/result-control-flow patching rather than protocol installation.
  - User clarified recovery was entered manually after the mode failed to boot recovery/Linux, so the captured boot props prove the failure state but should not be interpreted as a successful automatic recovery route.
- Harness update: `scripts/test-device-automatic.sh` now detects the common failure case where expected adb/recovery does not appear but the device has reset back into FastbootLib. It writes `fastboot-fallback.txt` with `fastboot devices` and `fastboot oem efi-status`, then exits with code 2 instead of waiting for manual interpretation.

## Stock boot/recovery retest

- User restored stock boot/recovery images, observed one red-screen boot, then an additional reboot reached stock recovery. User later reloaded custom recovery for log capture.
- Capture directory: `logs/20260508-202149_manual_mode-fakelocked-stock-images_vf91f6f3`.
- Strong evidence that `mode-fakelocked` works when the boot/recovery images verify cleanly:
  - `UefiLogSaved1` contains the staged fakelocked path on slot `_b`:
    - `gbl-chainload - FAKELOCKED - May  9 2026 00:32:05`
    - `ProtocolHookLib: fakelocked required hooks installed`
    - `vb-fakelock | READ_CONFIG | is_unlocked 1->0 | is_unlock_critical 1->0`
    - `fakelock=WRITE swallowed`
  - AVB result/state in the same saved boot:
    - `VB2: Authenticate complete! boot state is: green`
    - `VB2: boot state: green(0)`
    - `AvbSlotVerify returned OK`
    - `[AddOplusCmdLineFromVBCmdLineLen]: Adding  oplusboot.verifiedbootstate=green`
    - cmdline fragments include `oplusboot.mode=recovery` and `oplusboot.verifiedbootstate=green`.
  - The boot reached kernel handoff:
    - `Shutting Down UEFI Boot Services`
    - `Start EBS`
    - `Phoenix:Exit boot services success stage`
- The current pulled `/proc/bootconfig`, `/proc/cmdline`, `/proc/bootloader_log`, and `getprop.boot.txt` show orange/unlocked because they correspond to the later custom-recovery boot, not the earlier successful stock/fakelocked green boot:
  - `UefiLog1` / current `bootloader_log` show `OK_NOT_SIGNED`, `boot state is: orange`, and `oplusboot.verifiedbootstate=orange` for the custom recovery path.
- Interpretation: fakelock protocol hooks are sufficient for clean/stock images and produce green bootloader state through AVB and command line. Custom/unsigned recovery still needs either permissive AVB-internal handling or result/control-flow patching if we want it to boot while externally reporting green/locked.

## Combined fakelocked-debug no-EBS capture

- Added/build-tested a combined mode payload for deeper stock-ABL tracing without EBS:
  - Build command: `./scripts/build.sh --mode mode-fakelocked-debug --debug-variant no-ebs`
  - Artifact: `dist/mode-fakelocked-debug-no-ebs.efi`
  - Defines: `FAKELOCKED_DEBUG`, `GBL_DEBUG_NO_EBS=1`, `GBL_DEBUG_PHASE_FLUSH=1`.
  - Intended behavior: fakelock VerifiedBoot policy plus QSEE/SCM/VerifiedBoot debug hooks; EBS hook intentionally skipped.
- Capture directory: `logs/20260508-204459_manual_fakelocked-debug-no-ebs-stock-images_vcc8a48e`.
- Combined-mode entry/hook evidence in `UefiLogSaved4`:
  - `gbl-chainload - FAKELOCKED_DEBUG - May  9 2026 02:36:16`
  - `FakeLockedDebug default/no-key route: entering FastbootLib`
  - `BootFlow: GetCurrentSlotSuffix returned suffix='_b'`
  - `DynamicPatch: patch1-efisp-recursion OK`
  - `DynamicPatch: patch7-orange-screen OK`
  - `ProtocolHookLib: start`
  - `QseecomHook: installed ...`
  - `ScmHook: installed 5 of 5 slots`
  - `VerifiedBootHook: installed 10 of 10 slots`
  - `ProtocolHook: ebs skipped by no-ebs debug variant`
  - `ProtocolHookLib: finished — installed 3 of 3`
- Fakelock/green evidence from the stock recovery boot:
  - `UefiLogSaved2`: `VB2: Authenticate complete! boot state is: green`
  - `UefiLogSaved2`: `VB2: boot state: green(0)`
  - `UefiLogSaved2`: `AvbSlotVerify returned OK`
  - `UefiLogSaved2`: `oplusboot.verifiedbootstate=green`
  - `UefiLogSaved2`: `vb-fakelock | READ_CONFIG | is_unlocked 1->0 | is_unlock_critical 1->0`
  - `UefiLogSaved2`: `fakelock=WRITE swallowed`
- QSEE/SCM/KeyMaster evidence captured under combined hooks:
  - `qsee-km | cmd=0x00000202(READ_KM_DEVICE_STATE)` succeeds.
  - `qsee-km | cmd=0x00000201(SET_ROT)` sends RoT digest `44149b5df4f23466590b6e9888b75e618dbe07220a078efcca37ef6218e566c7`.
  - `qsee-km | cmd=0x00000208(SET_BOOT_STATE)` sends `isUnlocked=0`, `color=0` (GREEN), public key hash `8d897f62492ea617f777bad41a5711ab621fcac1efc1865b890328ee8c3853bb`, `sysVer=0x40000`, `sysSpl=0x9A4`.
  - SCM/SIP logs include `TZ_INFO_GET_SECURE_STATE` returning `secure_state=0x40` with decoded flags `secboot=0 shk=0 dbg_dis=0 rpmb=0 dbg_re=1`.
  - Secretkeeper/BCC/AVF path also appears after the green boot: `Secretkeeper app is loaded and ready to be used`, `BCC Generation is Success`, `Modifying reference VM DT to add AVF data`, `Modifying Host Kernel DT to add AVF data`.
- Current pulled `/proc/*` and `UefiLog1` again correspond to a later custom-recovery/orange boot, not the earlier stock/fakelocked-debug green boot:
  - `UefiLog1` shows `OK_NOT_SIGNED`, `boot state is: orange`, and `oplusboot.verifiedbootstate=orange`.

## Fakelock vs no-fakelock comparison report

- Wrote comparison report: `docs/re/fakelock-vs-debug-comparison.md`.
- Report compares:
  - fakelocked/debug/no-EBS stock-image capture: `logs/20260508-204459_manual_fakelocked-debug-no-ebs-stock-images_vcc8a48e`;
  - best available no-fakelock baseline: `logs/20260508-113317_manual_mode-debug_vebs-regression-patch-only-no-ebs-success`.
- Main verified deltas:
  - VerifiedBoot device-state read is post-mutated from unlocked to locked: `vb-fakelock | READ_CONFIG | is_unlocked 1->0 | is_unlock_critical 1->0`.
  - VerifiedBoot writes are swallowed: `fakelock=WRITE swallowed`.
  - KeyMaster `SET_BOOT_STATE` changes from `isUnlocked=1`, `color=1`, zero pubkey in no-fakelock baseline to `isUnlocked=0`, `color=0`, real pubkey hash under fakelock.
  - Oplus-visible state changes from `oplusboot.verifiedbootstate=orange` / `lock_state:0` to `oplusboot.verifiedbootstate=green` / `lock_state:1`.
  - SCM `TZ_INFO_GET_SECURE_STATE` remains `secure_state=0x40 ... dbg_re=1`, so fakelock does not alter low-level TZ secure-state reporting.
- Caveat captured in the report: the no-fakelock baseline is not a perfect same-slot stock-clean baseline; it contains orange/custom-ish recovery evidence (`OK_NOT_SIGNED`, `ERROR_VERIFICATION`) and mixed hook/no-hook rings. Future clean v2 should capture `mode-debug-no-ebs` and `mode-fakelocked-debug-no-ebs` on the same stock slot with no intervening custom-recovery boot.

## 2026-05-09 — Oplus fastboot/RPMB/reserve risk hypotheses

Prompting concern: users have reported that earlier EFISP-based mods such as `gbl_root_canoe` can permanently lose stock fastboot. We do not yet know whether this means the stock EFI/fastboot asset is overwritten, stock fastboot is merely gated by persistent state, or Android/OOS userspace later records/repairs inconsistent state.

Current local evidence:

- Oplus/Phoenix path is active in stock ABL boots and reads/writes reserve/boot-info state. Logs show `Phoenix:Read oplusreserve1`, `set_boot_info_to_rpmb`, and Oplus secure app traffic.
- Fakelock currently protects only the `QCOM_VERIFIEDBOOT_PROTOCOL` devinfo path: it mutates `VBRwDeviceState(READ_CONFIG)` and swallows VerifiedBoot writes/resets.
- Oplus RPMB boot-info writes are **not** blocked by fakelock. In current captures they receive fakelocked-visible state:
  - fakelock: `set_boot_info_to_rpmb: secureboot_state:1 lock_state:1 ...`
  - no-fakelock: `set_boot_info_to_rpmb: secureboot_state:1 lock_state:0 ...`
- SCM/TZ secure-state remains unchanged by fakelock (`secure_state=0x40 ... dbg_re=1`), but current SCM hook only logs fuse/rollback-looking calls; it does not drop them.
- `GENERATE_FRS_AND_UDS` is observed in the no-fakelock baseline, but the current reporting only summarizes it. Do not log raw UDS; log lengths/status/hashes only.

Public research signals to verify locally:

- Oplus/Oppo fastboot unlock/gating has community reports around an engineering `fastbootUnlock(byte[], int)` flow and device-stored public keys/certs under `/odm/etc/DownloadModeKey/`.
- Community reports suggest `oplusreserve`/`oplusreserve1` can store deep-testing / unlock / boot-mode state.
- Some OnePlus/Oppo device-specific guides claim ABL reads model/project or unlock-relevant data from RPMB and/or param/reserve partitions.
- No strong public evidence was found directly tying FRS/UDS/DICE to Oplus stock fastboot command gating, but those paths remain sensitive and should be classified.

Most likely mechanisms for “lost stock fastboot,” ranked:

1. Stock EFI/fastboot asset replacement: an EFISP/UEFI partition was overwritten or flashed to the wrong slot/label, so stock fastboot code is literally gone or bypassed.
2. Oplus RPMB boot-info poisoning: fakelock or stock ABL writes a persistent `lock_state`/boot-mode/crash/fastboot-disable field that later gates stock fastboot.
3. `oplusreserve1` / Phoenix state writes missed by current hooks: likely BlockIO/partition traffic rather than QSEE traffic.
4. SCM software fuse / rollback side effects: current hook logs candidates but does not prevent them.
5. `misc`/BCB/slot metadata confusion: device routes to recovery/fastbootd or a custom FastbootLib path, appearing as stock fastboot loss.
6. FRP/devinfo/persist gating: stock fastboot exists but hides/denies commands based on OEM unlock / FRP / critical-lock state.
7. OOS userspace repair writes after seeing inconsistent boot state: Android boots green/locked while low-level TZ/debug state still exposes unlocked/debug-like flags, then records something persistent.
8. KM/FRS/UDS damage: less likely to directly hide fastboot, but can break attestation/RKP/DICE and trigger rescue/repair behavior.

High-priority probes to add before more risky full boots:

- Decode OplusSec RPMB boot-info reads/writes in `QseecomHook.c`: log raw length, offset/value table, known fields (`secureboot_state`, `lock_state`, `boot_mode`, `tmp_boot_mode`) plus hashes, but avoid secret dumps.
- Add selected partition write monitoring for `oplusreserve1`, `oplusreserve2`, `oplusreserve3`, `misc`, `devinfo`, `frp`, `persist`, `metadata`, `param`, `uefi_a/b`, `abl_a/b`, `xbl_config_a/b`: label, LBA/offset, length, pre/post hash, first bytes, boot phase.
- Add boot-state snapshots before/after each test: fastboot identity/inventory, selected partition hashes, BCB/misc parse, slot metadata, Oplus RPMB boot-info decode.
- Strengthen SCM side-effect census for fuse/rollback/RPMB-provision-looking SIPs. Keep logging-only first; consider drop variants only after same-slot stock baseline classification.
- Add KeyMaster state pointer hashing for `READ_KM_DEVICE_STATE` / `WRITE_KM_DEVICE_STATE` and `GENERATE_FRS_AND_UDS` response hashing. Never print raw UDS.
- Separate bootloader-side from Android/OOS userspace writes: run stock recovery/fastboot log pulls before allowing OOS to boot; only later do a controlled OOS boot with before/after partition/RPMB diffs.

## 2026-05-09 — mode taxonomy and Phoenix/DICE RE targets

User clarified:

- EFISP mod users losing stock fastboot is likely caused by propagated persistent state or asset replacement, not something solved by repatching ABL to always show a fastboot screen.
- Global variant has stock fastboot available, so local device cannot reproduce deep-test gated stock-fastboot loss directly.
- Deep-test recovery research is ongoing externally; gbl-chainload-side goal is to understand/influence persistent state where possible.
- Mode plan should return to the original three-level concept:
  1. minimal VerifiedBoot/bootstate fakelock;
  2. TZ/KeyMaster/SCM spoof/drop mode;
  3. OEM/Oplus/RPMB/Phoenix-aware mode.

Mode taxonomy:

- **Mode 1 — minimal VB / bootstate fakelock**:
  - Current safe default.
  - Spoofs only `QCOM_VERIFIEDBOOT_PROTOCOL` state: locked reads/init; swallowed VB writes/resets.
  - Logs KM/Oplus/SCM effects but does not mutate QSEE/SCM/OplusSec traffic.
  - Works for stock/clean images; custom/unsigned images still need AVB result handling.

- **Mode 2 — TZ / KeyMaster / SCM spoof-drop**:
  - Debug/experimental only.
  - Candidate spoof/drop scope: coherent KeyMaster boot-state/secure-state presentation and confirmed SCM fuse/rollback drops.
  - Never mutate or dump raw `FBE_SET_SEED`, raw UDS, Secretkeeper/BCC/DICE secrets, RPMB provision/erase, or unknown SCMs.
  - Need exact SCM return semantics before returning fake success for `TZ_BLOW_SW_FUSE_ID` or rollback-update calls.

- **Mode 3 — OEM / Oplus / RPMB / Phoenix-aware**:
  - Missing safety layer for repeated OOS boots or user-distributable flows.
  - Requires decoding/virtualizing OplusSec RPMB boot-info and observing Phoenix reserve writes.
  - Must classify fields before blocking writes: `lock_state`, `boot_mode`, `tmp_boot_mode`, crash/rescue counters, deep-test/fastboot flags, checksums/MACs if any.

Static RE / instrumentation anchors:

- Fastboot entry/gating source-side anchors:
  - `edk2/QcomModulePkg/Application/LinuxLoader/LinuxLoader.c:420-487` — `BootIntoFastboot`, `LoadImageAndAuth` fallback, `FastbootInitialize()`.
  - `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c:4596-4668` — OEM escape and RPMB lock/unlock hooks.
  - `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c:4840-4899` — registered fastboot command table and vars.
- DICE / FRS / UDS / BCC source-side anchors:
  - `edk2/QcomModulePkg/Library/avb/KeymasterClient.c:748-858` — `KeyMasterGetFRSAndUDS()`, `KEYMINT_GENERATE_FRS_AND_UDS`, DevInfo FDR/FRS read-write, copies into BCC params.
  - `edk2/QcomModulePkg/Library/avb/AvbPopulateBccParams.c:127-159` — DICE mode selection and BCC param population.
  - `edk2/QcomModulePkg/Library/OpenDice/QcBcc.c:99-169` — BCC root construction from UDS/FRS and DICE derivation.
- Ghidra string targets for Oplus/Phoenix:
  - `Phoenix`, `oplusreserve1`, `set_boot_info_to_rpmb`, `secureboot_state`, `lock_state`, `boot_mode`, `tmp_boot_mode`, `DownloadModeKey`, `deep test`, `fastbootUnlock`, `OplusSecInvokeCommand`.

Next RE questions:

1. Where exactly does Oplus ABL write RPMB boot-info, and what is the full field layout?
2. What Phoenix/reserve fields are security or routing relevant?
3. Does fastboot loss in affected devices correlate with UEFI/EFISP asset hash changes, or only persistent state changes?
4. What are exact return/status semantics for SCM fuse/rollback calls if dropped?
5. What is the structure behind KM `READ_KM_DEVICE_STATE` / `WRITE_KM_DEVICE_STATE` pointers?
6. Can `GENERATE_FRS_AND_UDS` be broken down into FRS/UDS/BCC/DICE fields using edk2 layouts while logging only non-secret hashes?

## 2026-05-09 — recovery/dtbo embedded-vbmeta failure model

Important correction to AVB patch strategy: for custom recovery/dtbo, the first failure is often **parser/input validity**, not merely verification outcome.

Recovery and dtbo can carry embedded AVB footer/vbmeta data at fixed partition locations. A custom recovery flashed directly may wipe or zero the stock footer at `partition_size - AVB_FOOTER_SIZE` and the embedded vbmeta struct. In locked-path AVB this means ABL/libavb can fail while parsing the footer/vbmeta input (`AVBf` footer missing, invalid metadata) before reaching ordinary hash/signature outcome handling. This is why simply forcing final slot result to OK, or patching a hash mismatch branch, can still explode: the data path may never produce complete `SlotData` or digest inputs.

Implications:

- Baiqi/gbl_root_canoe transplant-style workarounds likely work because they make recovery/dtbo embedded vbmeta **parseable** again by grafting stock footer/vbmeta bytes, not because the custom recovery content actually matches stock digest.
- For our approach, the better target is an **AVB input façade**: ensure ABL/libavb sees parseable, stock-equivalent embedded vbmeta for recovery/dtbo while keeping actual custom recovery contents loadable in recovery mode.
- This avoids chasing `androidboot.*` / bootconfig outputs after the fact. If libavb receives coherent inputs, ABL naturally builds `androidboot.vbmeta.digest`, possible OEM per-partition digests, KeyMaster/VBH, and Oplus state.

Preferred patch direction for gbl-chainload/ABL patching:

1. Capture or embed stock recovery and dtbo embedded footer/vbmeta bytes before modification.
2. Intercept ABL/libavb reads for recovery/dtbo embedded footer/vbmeta metadata and return cached stock bytes on normal boot path.
3. Do not use cached recovery content for actual recovery boot; recovery mode should load real on-disk custom recovery contents.
4. Let ABL compute vbmeta digest and bootconfig/cmdline naturally from the substituted parseable metadata.
5. If a hash descriptor later compares recovery contents against stock digest, handle that as an outcome mismatch after parse succeeds; do not confuse this with missing-footer parser failure.
6. Verify on stock and patched boots whether OEM emits per-partition properties such as `androidboot.vbmeta.recovery.digest` or `androidboot.vbmeta.dtbo.digest` via `/proc/bootconfig` and `/proc/cmdline`.

Key distinction:

- Output patching: `AVB failed -> rewrite cmdline/bootconfig/KeyMaster/Oplus state` is fragile.
- Input façade: `AVB sees parseable trusted-looking metadata -> normal ABL code generates coherent outputs` is preferred.

Open RE tasks:

- Confirm whether recovery and dtbo are represented as chain descriptors or hash descriptors in this device's main vbmeta.
- Locate the exact ABL/libavb partition-read call(s) for recovery/dtbo footer and embedded vbmeta reads.
- Determine whether Oplus emits per-partition digest properties and where they are built.
- Determine whether Android init/Oplus userspace ever re-reads recovery/dtbo embedded vbmeta during normal boot; expectation is no, but this must be confirmed per firmware.

Follow-up log inventory confirms Android/Oplus emits per-boot-partition vbmeta digest properties in captured recovery userspace props:

- `ro.boot.vbmeta.boot.digest`
- `ro.boot.vbmeta.dtbo.digest`
- `ro.boot.vbmeta.init_boot.digest`
- `ro.boot.vbmeta.recovery.digest`
- `ro.boot.vbmeta.vendor_boot.digest`
- common fields: `ro.boot.vbmeta.digest`, `ro.boot.vbmeta.hash_alg`, `ro.boot.vbmeta.size`, `ro.boot.vbmeta.device_state`, `ro.boot.verifiedbootstate`, `ro.boot.veritymode`.

Representative logs:

- `logs/20260508-204459_manual_fakelocked-debug-no-ebs-stock-images_vcc8a48e/bootconfig:38-60` — common `androidboot.vbmeta.*` bootconfig fields.
- `logs/20260508-204459_manual_fakelocked-debug-no-ebs-stock-images_vcc8a48e/getprop.boot.txt:25-62` — per-partition `ro.boot.vbmeta.{boot,dtbo,init_boot,recovery,vendor_boot}.digest` fields.
- `logs/20260508-202149_manual_mode-fakelocked-stock-images_vf91f6f3/getprop.boot.txt:25-62` — same per-partition digest set.

This strengthens the input-façade requirement: if per-partition digests are derived by ABL/Oplus from embedded or descriptor vbmeta state, supplying cached stock parseable metadata at the AVB read layer is preferable to rewriting each output field.

Practical patch anchors from source/Ghidra mapping:

- `edk2/QcomModulePkg/Library/avb/libavb/avb_ops.c:154` — `AvbReadFromPartition()`, source-side analogue of the binary read hook.
- `edk2/QcomModulePkg/Library/avb/libavb/avb_slot_verify.c:699-728` — embedded footer read / validation.
- `edk2/QcomModulePkg/Library/avb/libavb/avb_slot_verify.c:748-793` — embedded vbmeta read and `avb_vbmeta_image_verify()`.
- `edk2/QcomModulePkg/Library/avb/libavb/avb_slot_verify.c:497-503` — hash descriptor digest mismatch branch after parse succeeds.

Build helper added:

- `scripts/extract-avb-embedded-vbmeta.py` extracts the stock AVB footer and embedded vbmeta struct from raw recovery/dtbo partition images.
- Optional output C header provides byte arrays for a development build of the AVB input-façade patch.
- Documentation: `docs/re/avb-input-facade.md`.
- Validation performed: `python3 -m py_compile scripts/extract-avb-embedded-vbmeta.py`, script `--help`, and `git diff --check` all passed.

Current implementation blocker:

- GhidraMCP was not connected during this step, so exact stock ABL RVAs/anchors for `AvbReadFromPartition` / footer-read callsites were not confirmed.
- Actual `patch9-avb-input-facade` should wait for Ghidra-confirmed binary anchors and stock recovery/dtbo raw images or generated cached blobs.

## 2026-05-09 — patch9 AVB locked recoverable-continue implementation

Implemented a first minimal binary `patch9-avb-locked-recoverable-continue` for the `0xBE000` `LinuxLoader_infiniti.efi` PE family in:

```text
GblChainloadPkg/Library/DynamicPatchLib/oem/oneplus_canoe.c
```

Purpose:

- Split two meanings that Qualcomm's `LoadImageAndAuthVB2()` stores in one source Boolean:
  - allow libavb to continue and return populated `SlotData` on recoverable verification failures;
  - report downstream boot state as unlocked/orange.
- Fakelock needs the former but not the latter: libavb should be permissive enough to continue on `result_should_continue()` results, while original `AllowVerificationError` (`w29`) remains false for boot state / KeyMaster / cmdline generation.

Static source evidence:

- `VerifiedBoot.c:1364` initializes `AllowVerificationError = IsUnlocked()`.
- `VerifiedBoot.c:1379-1381` derives `VerifyFlags` from `AllowVerificationError`.
- `VerifiedBoot.c:1591-1605` handles `AllowVerificationError && ResultShouldContinue(Result)`, fatal non-OK, and OK rollback-update.
- `avb_slot_verify.c:1558-1564` fails early without populated `SlotData` when `allow_verification_error` is false and result is non-OK.
- `avb_slot_verify.c:1672-1674` asserts non-allow mode only returns OK.

Binary evidence / rewrites in `/tmp/linuxloader_infiniti.dis` and `/home/vivy/gbl_root_canoe/images/LinuxLoader_infiniti.efi`:

- `0x25388`: `cset w24, ne` (`0x1A9F07F8`) → `mov w24, #1` (`0x52800038`)
  - Forces `VerifyFlags` bit 0 so libavb gets `AVB_SLOT_VERIFY_FLAGS_ALLOW_VERIFICATION_ERROR` and can return `SlotData` for recoverable failures.
  - Leaves `w29` unchanged for downstream locked/green state.
- `0x25A64`: `cbz w29, 0x25D58` (`0x340017BD`) → `cbz w24, 0x25D8C` (`0x34001958`)
  - Recovery-first AVB check: `Result==OK` uses existing locked OK path; non-OK falls through to the existing result classifier.
- `0x25C44`: `cbz w29, 0x25D04` (`0x3400061D`) → `cbz w24, 0x25DF8` (`0x34000DB8`)
  - Common post-AVB check: `Result==OK` still reaches normal rollback-update path; non-OK falls through to existing classifier/fatal path.
- Existing inline classifier at `0x25C50-0x25C68` uses `cmp w24,#5`, bitmask `0x39`, and accepts result values `{0,3,4,5}`. Fatal non-OK values still go to the original fatal log/status path.

Safety properties preserved:

- Does **not** rewrite `AvbSlotVerifyResult` to OK.
- Does **not** route forgiven non-OK results to the OK rollback-update branch.
- Fatal libavb results should remain fatal.
- Patch is included only for `FAKELOCKED`, `FAKELOCKED_DEBUG`, and host tests.
- Patch is mandatory in fakelocked builds and guarded by PE size, executable-section checks, and exact instruction signatures.

Known caveat:

- Continue/fatal logging may still use existing "State: Unlocked" strings on the non-OK classifier path because the patch intentionally reuses the existing unlocked classifier/log block instead of adding a larger helper. Downstream state should remain locked because `w29` is not changed.

Validation performed:

```text
bash ./tests/042_dynamic_patch_harness.sh
git diff --check
python3 -m py_compile scripts/extract-avb-embedded-vbmeta.py
./scripts/build.sh --mode mode-fakelocked
./scripts/build.sh --mode mode-fakelocked-debug --debug-variant no-ebs
./tests/runall.sh
```

Build note:

- Running both build commands in parallel races the shared `Build/` directory and can produce transient `ar: unable to copy file ... No such file or directory` failures. Sequential builds succeeded.

Runtime validation still needed before relying on patch9 on-device:

1. Stage `mode-fakelocked` or `mode-fakelocked-debug-no-ebs` only with user approval.
2. Confirm boot log shows `DynamicPatch: patch9-avb-locked-recoverable-continue OK`.
3. On a modified recovery/dtbo case, confirm real non-OK AVB result is logged but boot continues.
4. Confirm no `VB2: UpdateRollbackIndex flag set` for forgiven non-OK.
5. Confirm final state remains locked/green: KeyMaster `isUnlocked=0`, `color=0`, bootconfig/props locked/green.
6. Negative test later: corrupt footer/metadata should still fail as `INVALID_METADATA`/hard failure.

### 2026-05-09 follow-up: reported patch9 miss / Ghidra and host fixture audit

Pulled recovery/logfs evidence after the reported mandatory patch9 miss:

```text
logs/20260509-012451_manual_patch9-mandatory-miss_vcc8a48e/
```

Important finding: the captured rings are not from the current fakelocked/patch9 payload. They show the old auto-debug payload and no fakelock hooks:

- `GblChainload_Boot3.txt` / `GblChainload_Boot4.txt`: `gbl-chainload - AUTO_DEBUG_MODE - May  8 2026 18:25:36`
- `UefiLogSaved3.txt` / `UefiLogSaved4.txt`: `gbl-chainload - AUTO_DEBUG_MODE - May  8 2026 18:25:40`
- Dynamic patch table only includes/applies two patches in those logs:
  - `DynamicPatch: patch1-efisp-recursion OK`
  - `DynamicPatch: patch7-orange-screen OK`
  - `DynamicPatchLib: finished — applied=2 missed=0 worst=0`
- The same rings say `BootFlow: protocol hooks skipped for this mode`.

Interpretation: this specific capture does **not** prove patch9 missed in a current `FAKELOCKED`/`FAKELOCKED_DEBUG` payload. It proves the device booted an older `AUTO_DEBUG_MODE` image where patch9 was not compiled into `kOemPatches[]`.

Ghidra status:

- The OpenCode `ghidra-mcp_*` tool wrapper still reports `Not connected`.
- Ghidra GUI and plugin are alive, and the Unix socket works directly:
  - socket: `/run/user/1000/ghidra-mcp/ghidra-649179.sock`
  - `/mcp/schema` returns OK.
  - current program: `LinuxLoader_infiniti.efi`.
- Direct socket queries confirmed `FUN_00025340` spans `00025340-00027797` and contains patch9 sites.
- Applied visible Ghidra annotations and saved the program:
  - disassembly comments + `patch9` bookmarks at `0x25388`, `0x25a64`, `0x25c44`.
  - plate comment on `FUN_00025340` documenting the patch9 evidence and intent.

Host fixture audit:

- Real fixture: `/home/vivy/gbl_root_canoe/images/LinuxLoader_infiniti.efi`
- Size: `0xBE000`.
- Patch9 signature words all match:
  - `0x25388 = 0x1A9F07F8`
  - `0x25A64 = 0x340017BD`
  - `0x25C44 = 0x3400061D`
  - `0x25D58 = 0x340001B8`
  - `0x25D04 = 0x340007B8`
- PE executable section check passes for patch targets (`.text` raw `0x1000`, size `0x7a000`).
- A direct host apply of all patch tables with `-DFAKELOCKED -DGBL_PATCH_HOST_TEST` against the real fixture reports:
  - `patch1-efisp-recursion -> PATCH_OK`
  - `patch7-orange-screen -> PATCH_OK`
  - `patch8-update-dtb-log-helper -> PATCH_OK` in host-test mode only
  - `patch9-avb-locked-recoverable-continue -> PATCH_OK`
  - post bytes: `0x25388=0x52800038`, `0x25A64=0x34001958`, `0x25C44=0x34000DB8`.

Test update:

- `tests/042_dynamic_patch_harness.sh` now includes an optional real-fixture patch9 test when `/home/vivy/gbl_root_canoe/images/LinuxLoader_infiniti.efi` exists.
- Validation after this update:
  - `bash ./tests/042_dynamic_patch_harness.sh`
  - `./tests/runall.sh`
  - `git diff --check`

Next runtime requirement:

- Re-stage the freshly built `dist/mode-fakelocked.efi` or `dist/mode-fakelocked-debug-no-ebs.efi`; do not use the stale `AUTO_DEBUG_MODE` payload.
- The first proof line should be `gbl-chainload - FAKELOCKED` or `FAKELOCKED_DEBUG`, followed by `DynamicPatch: patch9-avb-locked-recoverable-continue OK`.

## 2026-05-09 — AVB descriptor dump from EU 16.0.5.703 firmware

Firmware inspected:

```text
~/Downloads/RegionalHybrid Flasher 15 EU 16.0.5.703/OOS_FILES_HERE
```

Tooling:

```text
~/android/fox_14.1/external/avb/avbtool.py
scripts/extract-avb-embedded-vbmeta.py
```

Descriptor report written:

```text
docs/re/avb-descriptor-findings-eu-16.0.5.703.md
```

Key findings:

- Main `vbmeta.img` has chain partition descriptors for `boot`, `dtbo`, `recovery`, `vbmeta_system`, and `vbmeta_vendor`.
- Main `vbmeta.img` has direct hash descriptors for `init_boot` and `vendor_boot`.
- `boot.img`, `recovery.img`, `dtbo.img`, `init_boot.img`, `vendor_boot.img`, and `pvmfw.img` all carry AVB footers/embedded vbmeta in this firmware set.
- The observed per-partition bootconfig digests match AVB descriptor digests, not obvious Oplus hardcoded additions:
  - boot digest matches `boot.img` embedded vbmeta hash descriptor.
  - dtbo digest matches `dtbo.img` embedded vbmeta hash descriptor.
  - pvmfw digest matches `vbmeta_system.img` pvmfw hash descriptor.
  - init_boot/vendor_boot digests match main `vbmeta.img` hash descriptors.
- The recovery digest in captured live logs (`0cd944a7...`) does not match this EU stock `recovery.img` descriptor (`26d30936...`), likely because the live capture came from a later custom recovery or a different regional image set.
- Extracted cached blob artifacts under `build/avb-cache/eu-16.0.5.703/` for boot/recovery/dtbo footers and embedded vbmeta.

Conclusion: the `androidboot.vbmeta.<partition>.digest` bootconfig fields are best treated as Qualcomm/AVB dynamic outputs derived from loaded AVB descriptors/partition data. Oplus-specific handling is separate (`oplusboot.*`). This supports the input-façade approach rather than output rewriting.
