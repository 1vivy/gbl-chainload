# Fakelocked vs. no-fakelock ABL behavior comparison

Date: 2026-05-09  
Branch context: `mode-fakelocked` / commit family around `cc8a48e`  
Scope: compare a fakelocked downstream ABL view against the best available no-fakelock debug baseline, with emphasis on AVB state, KeyMaster/TZ traffic, Qualcomm BSP/CLO-laid behavior, and Oplus/OEM additions.

## Executive summary

The fakelocked path changes the downstream bootloader-visible security state without changing the broad Qualcomm/Oplus boot pipeline.

Most of the boot sequence remains the same:

- ABL loads the same slot family (`abl_b` or `abl_a` depending on capture).
- Qualcomm/CLO-style AVB2 flow still loads `vbmeta`, `boot`, `dtbo`, `recovery`, `pvmfw`, `vbmeta_system`, `vbmeta_vendor`, `init_boot`, and `vendor_boot`.
- Oplus/Phoenix additions still append bootconfig/cmdline fields, write Phoenix reserve logs, run Oplus secure commands, generate BCC, and patch AVF data into DT.
- TZ/QSEE app traffic still goes through the same KeyMaster and Secretkeeper-ish paths.

The meaningful fakelock deltas are concentrated in VerifiedBoot device state and KeyMaster boot-state reporting:

- `VBRwDeviceState(READ_CONFIG)` is post-mutated from unlocked to locked.
- `VBRwDeviceState(WRITE_CONFIG)` is swallowed.
- KeyMaster `SET_BOOT_STATE` receives `isUnlocked=0` and `color=0` (GREEN) instead of `isUnlocked=1` and `color=1` (ORANGE).
- Oplus cmdline receives `oplusboot.verifiedbootstate=green` instead of `orange`.
- Oplus RPMB boot-info write sees `lock_state:1` instead of `lock_state:0`.

The biggest caveat: the available no-fakelock baseline is not a perfect stock-image clean baseline. It is a no-fakelock `MODE_DEBUG`/no-EBS-style capture with orange/custom-ish recovery behavior. It is still useful for state deltas and protocol traffic, but a cleaner future comparison should capture `FAKELOCKED_DEBUG no-EBS` and `MODE_DEBUG no-EBS` against the exact same stock boot/recovery images and slot.

## Captures compared

### Fakelocked/debug/no-EBS capture

Path:

```text
logs/20260508-204459_manual_fakelocked-debug-no-ebs-stock-images_vcc8a48e
```

Relevant files:

- `logfs/UefiLogSaved4.txt` — combined payload entry, hook installation, QSEE/SCM/KeyMaster debug traffic.
- `logfs/UefiLogSaved2.txt`, `UefiLogSaved0.txt`, `UefiLogSaved1.txt` — stock/fakelocked green recovery boot records.
- `UefiLog1.txt`, `bootloader_log`, `/proc/*` captures — later custom recovery/orange boot; useful as a warning not to overinterpret current `/proc` as the earlier green boot.

Mode evidence:

```text
UefiLogSaved4:890  gbl-chainload - FAKELOCKED_DEBUG - May  9 2026 02:36:16
UefiLogSaved4:947  QseecomHook: installed ...
UefiLogSaved4:948  ScmHook: installed 5 of 5 slots
UefiLogSaved4:949  VerifiedBootHook: installed 10 of 10 slots
UefiLogSaved4:950  ProtocolHook: ebs skipped by no-ebs debug variant
UefiLogSaved4:951  ProtocolHookLib: finished — installed 3 of 3
```

### Best available no-fakelock baseline

Path:

```text
logs/20260508-113317_manual_mode-debug_vebs-regression-patch-only-no-ebs-success
```

Relevant files:

- `logfs/UefiLogSaved3.txt` — no-fakelock `MODE_DEBUG` path through patched ABL and orange recovery.
- `logfs/UefiLogSaved4.txt` / `UefiLogSaved2.txt` — richer QSEE/SCM/KeyMaster hook traffic around the same no-fakelock/orange behavior.

Mode evidence:

```text
UefiLogSaved3:1046  gbl-chainload - MODE_DEBUG - May  8 2026 17:22:02
UefiLogSaved3:1078  BootFlow: debug patch-only variant - protocol hooks skipped
```

Additional no-fakelock debug-hook evidence exists in the same capture set:

```text
UefiLogSaved4:820  ProtocolHookLib: start
UefiLogSaved4:821  QseecomHook: installed ...
UefiLogSaved4:822  ScmHook: installed 5 of 5 slots
UefiLogSaved4:823  VerifiedBootHook: installed 10 of 10 slots
```

This mismatch within the baseline directory is why the baseline is marked partial: it contains both patch-only/no-hooks and hook-debug views across nearby saved rings.

## Boot path and mode behavior

### Common path

Both captures follow the same top-level sequence:

1. Stage EFI payload from FastbootLib.
2. `oem boot-efi` starts payload.
3. Payload loads ABL from active slot.
4. Dynamic patches apply:
   - `patch1-efisp-recursion` rewrites `efisp` → `nulls`.
   - `patch7-orange-screen` rewrites the orange warning branch.
5. Payload starts patched ABL via `LoadImage`/`StartImage`.
6. ABL proceeds through Qualcomm/Oplus boot path.

Fakelocked/debug capture:

```text
UefiLogSaved4:924  BootFlow: start
UefiLogSaved4:926  BootFlow: GetCurrentSlotSuffix returned suffix='_b'
UefiLogSaved4:927  BootFlow: loading patched ABL from abl_b
UefiLogSaved4:936  Patch1 (EFISP): rewrote 'efisp' -> 'nulls' at offset 0x6F2D6
UefiLogSaved4:938  Patch7 (orange): CBZ at 0x78F0 W10 -> B
UefiLogSaved4:955  BootFlow: finished — handing off to patched ABL
```

No-fakelock baseline:

```text
UefiLogSaved3:1058  BootFlow: start
UefiLogSaved3:1060  BootFlow: GetCurrentSlotSuffix returned suffix='_a'
UefiLogSaved3:1061  BootFlow: loading patched ABL from abl_a
UefiLogSaved3:1070  Patch1 (EFISP): rewrote 'efisp' -> 'nulls' at offset 0x6F2D6
UefiLogSaved3:1072  Patch7 (orange): CBZ at 0x78F0 W10 -> B
UefiLogSaved3:1082  BootFlow: finished — handing off to patched ABL
```

Relevance: the dynamic patch layer is not what creates green/locked. It is common infrastructure. The fakelock state change happens through VerifiedBoot protocol policy.

## AVB result/state comparison

### Fakelocked stock-image boot

The stock/fakelocked run verifies cleanly and reports green:

```text
UefiLogSaved2:26   VB2: UpdateRollbackIndex flag set
UefiLogSaved2:27   VB2: UpdateRollbackIndex done.
UefiLogSaved2:43   VB2: Authenticate complete! boot state is: green
UefiLogSaved2:53   VB2: boot state: green(0)
UefiLogSaved2:79   AvbSlotVerify returned OK
```

The loaded partition set is normal for recovery boot:

```text
UefiLogSaved2:48  Loaded Partition: boot
UefiLogSaved2:49  Loaded Partition: dtbo
UefiLogSaved2:50  Loaded Partition: recovery
UefiLogSaved2:51  Loaded Partition: pvmfw
```

### No-fakelock baseline

The no-fakelock baseline reports orange and tolerates verification errors because the device is truly unlocked:

```text
UefiLogSaved3:1132  avb_slot_verify.c:500: ERROR: boot_a: Hash of data does not match digest in descriptor.
UefiLogSaved3:1139  avb_slot_verify.c:872: ERROR: recovery_a: Error verifying vbmeta image: OK_NOT_SIGNED
UefiLogSaved3:1165  VB2: Authenticate complete! boot state is: orange
```

Earlier in the same ring, the post-AVB result is explicitly not OK:

```text
UefiLogSaved3:22  avb_slot_verify.c:872: ERROR: recovery_a: Error verifying vbmeta image: OK_NOT_SIGNED
UefiLogSaved3:29  AvbSlotVerify returned ERROR_VERIFICATION
```

### Interpretation

The state change is not just a cosmetic string patch. In the stock-image fakelocked case, AVB naturally succeeds (`OK`) and the fakelock policy ensures downstream state reports locked/green. In the custom/unsigned case, no amount of fakelock alone can make `OK_NOT_SIGNED` disappear; a separate AVB permissive/result-control patch would be needed to boot custom recovery while externally reporting green.

## VerifiedBoot protocol deltas

### Fakelocked behavior

The combined fakelock/debug capture proves protocol-level mutation:

```text
UefiLogSaved4:966  VB: RWDeviceState: Succeed using rpmb!
UefiLogSaved4:967  vb-fakelock | READ_CONFIG | is_unlocked 1->0 | is_unlock_critical 1->0
UefiLogSaved4:968  vb-rwstate | op=READ | bufLen=3344 | ... | st=Success
```

Writes are suppressed:

```text
UefiLogSaved4:12   vb-rwstate | op=WRITE | ... | st=Success | fakelock=WRITE swallowed
UefiLogSaved2:47   vb-rwstate | op=WRITE | ... | st=Success | fakelock=WRITE swallowed
```

### No-fakelock behavior

No-fakelock baseline reads real RPMB/devinfo state and does not log `vb-fakelock` transitions:

```text
UefiLogSaved3:1085  VB: RWDeviceState: Succeed using rpmb!
UefiLogSaved3:1089  VB: RWDeviceState: Succeed using rpmb!
UefiLogSaved3:1090  VB: RWDeviceState: Succeed using rpmb!
```

In no-fakelock debug-hook rings, KeyMaster write/device-state commands are visible and not swallowed:

```text
UefiLogSaved4:861  qsee-km | cmd=0x00000203(WRITE_KM_DEVICE_STATE) | ... | st=Success
```

### Relevance

This is the core mechanism. The fakelock payload changes what ABL sees after it reads the physical/RPMB-backed device-info structure. It does not change the upstream device lock state or require persistent RPMB lock writes.

## KeyMaster / QSEE comparison

### Fakelocked KeyMaster state

Fakelocked KeyMaster `SET_BOOT_STATE` is locked/green:

```text
UefiLogSaved4:102  qsee-km | cmd=0x00000200(probe) | ... | ver=3.0.3 | buildId=0x7C | st=Success
UefiLogSaved4:106  qsee-km | cmd=0x00000201(SET_ROT) | ... | rotDigest=44149b5df4f23466590b6e9888b75e618dbe07220a078efcca37ef6218e566c7 | st=Success
UefiLogSaved4:111  qsee-km | cmd=0x00000208(SET_BOOT_STATE) | ... | isUnlocked=0 | pubKey=8d897f62492ea617f777bad41a5711ab621fcac1efc1865b890328ee8c3853bb | color=0 | sysVer=0x40000 | sysSpl=0x9A4 | st=Success
```

### No-fakelock KeyMaster state

No-fakelock KeyMaster `SET_BOOT_STATE` is unlocked/orange:

```text
UefiLogSaved4:50  qsee-km | cmd=0x00000200(probe) | ... | ver=3.0.3 | buildId=0x7C | st=Success
UefiLogSaved4:54  qsee-km | cmd=0x00000201(SET_ROT) | ... | rotDigest=4bf5122f344554c53bde2ebb8cd2b7e3d1600ad631c385a5d7cce23c7785459a | st=Success
UefiLogSaved4:59  qsee-km | cmd=0x00000208(SET_BOOT_STATE) | ... | isUnlocked=1 | pubKey=0000000000000000000000000000000000000000000000000000000000000000 | color=1 | sysVer=0x40000 | sysSpl=0x9A4 | st=Success
```

### Interpretation

This is the strongest end-to-end proof that fakelock is more than an Android cmdline change:

- No-fakelock/orange uses empty public key and `isUnlocked=1`.
- Fakelock/green uses real public key digest material and `isUnlocked=0`.
- The Trusted App receives the green/locked boot state via normal KeyMaster command `0x208`.

## SCM / SIP / secure-state comparison

Both captures repeatedly call `TZ_INFO_GET_SECURE_STATE` and decode the same secure-state flags:

Fakelocked:

```text
UefiLogSaved4:962  scm-sip | smcid=0x02000604(TZ_INFO_GET_SECURE_STATE) | tz_st=1 | secure_state=0x00000040 | status_1=0x00000000 | secboot=0 shk=0 dbg_dis=0 rpmb=0 dbg_re=1 | st=Success
```

No-fakelock:

```text
UefiLogSaved4:61   scm-sip | smcid=0x02000604(TZ_INFO_GET_SECURE_STATE) | tz_st=1 | secure_state=0x00000040 | status_1=0x00000000 | secboot=0 shk=0 dbg_dis=0 rpmb=0 dbg_re=1 | st=Success
```

Interpretation: fakelock is not changing the low-level TZ secure-state response. It is changing ABL/AVB/KeyMaster-facing boot state above that layer.

Other SCM/SIP behavior appears common:

```text
TZ_INFO_GET_FEATURE_VERSION_ID feature_id=0xA version=0x1402000
TZ_UPDATE_ROLLBACK_VERSION_IF_AB_PARTITION_FEATURE_ENABLED_ID p0=0x0
```

No-fakelock baseline has additional logged fuse calls:

```text
UefiLogSaved4:113  scm-sip | smcid=0x02000801(TZ_BLOW_SW_FUSE_ID) | fuse_id=0x0 | LOG-ONLY(NOT-DROPPED) | st=Success
UefiLogSaved4:114  scm-sip | smcid=0x02000801(TZ_BLOW_SW_FUSE_ID) | fuse_id=0x17 | LOG-ONLY(NOT-DROPPED) | st=Success
```

These are relevant as OEM/TZ side effects to monitor, but no evidence here says fakelock newly introduces them.

## UDS / FRS / DICE / BCC observations

The no-fakelock baseline includes a direct KeyMaster UDS/FRS-related command:

```text
UefiLogSaved4:128  qsee-km | cmd=0x00000219(GENERATE_FRS_AND_UDS) | h=4294901761 | fdrFlag=0x0 | frsSecLen=32 | st=Success | DO-NOT-MUTATE
```

The fakelocked-debug stock capture did not expose this exact `0x219` line in the strongest green section, but it does show downstream BCC/AVF success:

```text
UefiLogSaved2:174  Secretkeeper app is loaded and ready to be used
UefiLogSaved2:180  BCC Generation is Success
UefiLogSaved2:181  Modifying reference VM DT to add AVF data
UefiLogSaved2:209  Modifying Host Kernel DT to add AVF data
```

Interpretation:

- BCC/AVF path still runs under fakelock.
- The currently captured fakelocked-debug ring may not include the exact UDS/FRS command window, or the path differs under stock-clean green state.
- If UDS is a primary target, add or preserve earlier QSEE/KeyMaster logging around `cmd=0x219` and run a clean same-slot baseline pair.

## Qualcomm BSP / CLO-laid behavior vs OEM/Oplus additions

### Qualcomm / CLO-laid behavior

These are the lower-level and broadly Qualcomm-style pieces that appear mostly unchanged:

- ABL image loading and AVB2 partition verification.
- Rollback index handling:
  - `ReadPersistentValue 0xE`
  - `VB2: UpdateRollbackIndex flag set`
  - `VB2: UpdateRollbackIndex done`
- KeyMaster app load/probe and `SET_ROT` / `SET_BOOT_STATE` command forms.
- SCM/SIP calls such as `TZ_INFO_GET_SECURE_STATE`.
- PVMFW partition loading.
- BCC and AVF device-tree augmentation.

Representative fakelocked stock-clean lines:

```text
UefiLogSaved2:20  Load Image pvmfw_b total time: 2 ms
UefiLogSaved2:26  VB2: UpdateRollbackIndex flag set
UefiLogSaved2:27  VB2: UpdateRollbackIndex done.
UefiLogSaved2:180 BCC Generation is Success
UefiLogSaved2:181 Modifying reference VM DT to add AVF data
UefiLogSaved2:209 Modifying Host Kernel DT to add AVF data
```

### OEM / Oplus additions

Oplus/Phoenix code surrounds the Qualcomm path and is largely unchanged except for the state values it receives from AVB:

- Bootconfig construction:
  - `OplusPrjNameBootconfig`
  - `OplusHardwareSkuBootconfig`
  - `OplusChipEcidBootconfig`
  - `OplusFstabBootconfig`
- Cmdline construction:
  - `AddOppoBootModeCmdLineLen`
  - `Phoenix:Read oplusreserve1`
  - `OppoPrjNameCmdLine`
  - `OplusHardwareSkuCmdLine`
  - `AddOplusCmdLineFromVBCmdLineLen`
- Display/panel and charger/vendor setup.
- Phoenix bootloader-log lifecycle and reserve writes.
- Oplus secure boot-info write:
  - `set_boot_info_to_rpmb: secureboot_state:1 lock_state:<X> boot_mode:<Y>`

The state-sensitive OEM delta is visible here:

Fakelocked:

```text
UefiLogSaved2:34   set_boot_info_to_rpmb: secureboot_state:1 lock_state:1 boot_mode:1 tmp_boot_mode:0
UefiLogSaved2:145  [AddOplusCmdLineFromVBCmdLineLen]: Adding  oplusboot.verifiedbootstate=green
```

No-fakelock:

```text
UefiLogSaved3:93  [AddOplusCmdLineFromVBCmdLineLen]: Adding  oplusboot.verifiedbootstate=orange
UefiLogSaved3:1156 set_boot_info_to_rpmb: secureboot_state:1 lock_state:0 boot_mode:1 tmp_boot_mode:0
```

Interpretation: Oplus code consumes the fakelocked AVB state and propagates it into OEM cmdline/reserve/RPMB boot-info paths. The OEM machinery itself is not bypassed.

## Android-facing state comparison

Fakelocked green boot line fragments:

```text
UefiLogSaved2:145  [AddOplusCmdLineFromVBCmdLineLen]: Adding  oplusboot.verifiedbootstate=green
UefiLogSaved2:160-164 Cmdline: ... oplusboot.mode=recovery ... oplusboot.verifiedbootstate=green ...
```

No-fakelock baseline line fragments:

```text
UefiLogSaved3:93   [AddOplusCmdLineFromVBCmdLineLen]: Adding  oplusboot.verifiedbootstate=orange
UefiLogSaved3:108-112 Cmdline: ... oplusboot.mode=recovery ... oplusboot.verifiedbootstate=orange ...
```

Important caution: current pulled `/proc/bootconfig`, `/proc/cmdline`, and `getprop.boot.txt` in the fakelocked-debug capture show orange/unlocked because those files are from the later custom recovery boot used for log collection. The earlier stock/fakelocked green state is in saved UEFI rings, not the live `/proc` snapshot.

## What changes vs what stays the same

| Area | No-fakelock baseline | Fakelocked-debug no-EBS | Relevance |
| --- | --- | --- | --- |
| Payload identity | `MODE_DEBUG` | `FAKELOCKED_DEBUG` | Confirms comparison modes. |
| EBS hook | patch-only/no-hooks in one ring; debug hooks in another | debug hooks with `ebs skipped by no-ebs` | EBS intentionally excluded in combined mode. |
| ABL patching | patch1 + patch7 | patch1 + patch7 | Common infrastructure; not the source of green state. |
| Active slot | `_a` in baseline | `_b` in fakelocked capture | Imperfect comparison; future pair should same-slot. |
| AVB verification result | `OK_NOT_SIGNED` / hash errors; `ERROR_VERIFICATION` in parts of capture | `AvbSlotVerify returned OK` for stock recovery | Stock images remove need for AVB bypass. |
| AVB boot state | ORANGE | GREEN | Primary fakelock output. |
| DeviceInfo read | real unlocked | read post-mutated `1->0` | Core fakelock mechanism. |
| DeviceInfo write/reset | allowed/real | swallowed | Prevents persistent state mutation. |
| KeyMaster `SET_BOOT_STATE` | `isUnlocked=1`, `color=1`, zero pubkey | `isUnlocked=0`, `color=0`, real pubkey hash | Strongest TZ-visible proof. |
| Oplus cmdline | `oplusboot.verifiedbootstate=orange` | `oplusboot.verifiedbootstate=green` | OEM consumes VB state. |
| Oplus RPMB boot info | `lock_state:0` | `lock_state:1` | OEM state propagation. |
| TZ secure-state SIP | `secure_state=0x40 ... dbg_re=1` | same | Low-level TZ state unchanged. |
| Secretkeeper/BCC/AVF | present | present | Fakelock does not break this path. |

## Data quality and gaps

Known limitations:

1. **No perfect no-fakelock stock/green baseline yet.**  
   The best no-fakelock baseline is useful, but it includes orange/custom-ish recovery behavior and mixed rings.

2. **Different slots.**  
   Baseline uses `_a`; fakelocked-debug stock capture uses `_b`. Most mechanisms are slot-independent, but partition content differs.

3. **Live `/proc` snapshots are from later boots.**  
   Saved UEFI logs are the authoritative evidence for earlier fakelocked green boots.

4. **UDS/FRS capture is stronger in no-fakelock logs than fakelocked logs.**  
   The no-fakelock baseline has `GENERATE_FRS_AND_UDS`; the fakelocked-debug stock capture mainly proves BCC/AVF success and KeyMaster green state.

Recommended next capture for a clean report v2:

1. Flash stock boot/recovery on one chosen slot only.
2. Capture `mode-debug-no-ebs` on that same slot.
3. Capture `mode-fakelocked-debug-no-ebs` on that same slot.
4. Pull logs immediately after each, with no intervening custom recovery boot if possible; otherwise label live `/proc` as post-hoc only.

## Conclusions

1. Fakelock changes the downstream AVB/KeyMaster/Oplus-visible state from unlocked/orange to locked/green when images verify cleanly.
2. The low-level Qualcomm/TZ secure-state call remains unchanged; fakelock does not make the device physically locked upstream.
3. Oplus OEM additions faithfully consume and propagate the altered VerifiedBoot state into cmdline and RPMB boot-info paths.
4. Custom/unsigned recovery still needs separate AVB verification/result-control patches if it must boot while externally reporting green.
5. `FAKELOCKED_DEBUG + no-EBS` is the right capture mode for future protocol-level data: it records QSEE/SCM/VB traffic while avoiding the unstable EBS hook lane.
