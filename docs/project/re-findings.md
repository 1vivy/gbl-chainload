# Reverse-engineering findings

This file preserves distilled facts only. Session transcripts are intentionally not kept as durable project docs.

## Mode-0 and mode-1 behavior

- Mode-1 fakelocks the `QCOM_VERIFIEDBOOT_PROTOCOL` view: reads report locked state, writes/reset are swallowed or normalized, and clean stock images can present locked/green state to ABL/KM.
- Mode-0 is the honest unlocked observation path plus universal preservation baseline; it is not identical to a clean stock baseline in every capture.
- Mode-1 handles stock recovery + custom system, but it does not by itself make arbitrary custom recovery metadata acceptable to userspace AVB.

Evidence to preserve:

- Mode-1 mutation was designed around verified-boot protocol state rather than blanket ABL state rewrites.
- The old “fakelocked vs debug” comparison showed that no-fakelock captures and stock captures should not be conflated.

## Reserve token / `oplusreserve1`

- The critical preserve target is the DeepTest / fastboot token block at `oplusreserve1` `LastBlock - 0x3a5`, 4 KiB LBA `1114`, offset `0x45a000`.
- User locked/unlocked diffs show that the lock path zeroes this block.
- `UpdateUnlockRecord` at `LastBlock - 0x35c` / LBA `1187` is accounting/state, not the primary fastboot authorization gate.
- `oplusreserve1` has other writers, so a blanket write-swallow is broader than token preservation. The safe policy is targeted token-block preservation plus narrow accounting/state handling when needed.

Evidence to preserve:

- Token preservation policy is grounded in a locked/unlocked raw image diff, not only static strings.
- The writer table included non-unlock writers such as charge, UART, and DDR-control paths; this is why blanket partition-wide swallowing is too broad for a final policy.
- Return-value handling for swallowed writes must keep callers on their expected success path.

## Fastboot authorization gate

- Fastboot authorization centers on `ReadSecurityState` bits plus token/OCDT/model paths.
- `FastbootUnlockVerify` is the single gate callsite identified in static analysis.
- UI-confirmed lock path zeroes the token block; direct/no-display `fastboot flashing lock` path did not show the same token-zero behavior in the reviewed evidence.
- Preserve strategy should prioritize token block preservation and, if needed, present a coherent locked-looking UnlockRecord; it should not treat UnlockRecord as the primary gate.

Evidence to preserve:

- The static close-out identified `FastbootUnlockVerify` as the gate callsite to remember.
- Security-state bits, OCDT/model data, and token state are coupled; changing only one surface may produce inconsistent behavior.

## Recovery normal-boot failure

- Custom recovery under mode-1 can complete recovery boot while failing normal Android boot.
- The failure is after ABL: userspace AVB / first-stage init re-reads the on-disk recovery metadata and rejects missing, invalid, or mismatched in-partition AVB footer/vbmeta.
- The selected fix is disk-side grafting of stock recovery vbmeta/footer bytes onto the custom recovery image, either host-side or device-side.

Evidence to preserve:

- Graft offset is `round_up(custom_image_size, 4 KiB)` for the custom recovery payload, then append or splice the stock recovery vbmeta/footer bytes so userspace AVB reads satisfiable metadata.
- AOSP references used during investigation: `first_stage_mount.cpp` `InitAvbHandle()`, `fs_avb.cpp` `AvbHandle::Open()`, `avb_ops.cpp` `FsManagerAvbOps`, and `avb_slot_verify.c` hash descriptor verification.
- `patch10` can force libavb success inside ABL, but that does not remove the later userspace AVB read of recovery metadata.

## Patch anchors and fixtures

- `patch6` lock-state fastboot gate anchors on refusal strings such as “Flashing is not allowed in Lock State”, “Erase is not allowed in Lock State”, “Slot Change is not allowed in Lock State”, and “Snapshot Cancel is not allowed in Lock State”. The rewrite is on the preceding branch gate, not on the strings themselves.
- `patch10` anchors on the unique libavb string `Persistent values required for AVB_HASHTREE_ERROR_MODE_MANAGED_RESTART_AND_EIO`, walks back to the `avb_slot_verify` PACIASP function entry, forces allow-verification-error at entry, and forces OK at exit.
- `patch7` orange-state CBZ. The guard instruction is byte-identical across builds: `0x3400046A` = `CBZ W10, +0x8C` (imm19=35), rewritten to `0x14000023` (`B` same displacement). What moves between OTA builds is its surrounding context, so an EU-derived fixed byte anchor missed 201 (an extra `STR` shifts the CBZ from CSEL+4 to CSEL+8). `ApplyOrangeScreen` is **string-anchored**: scan for the warning text `"Your device has been unlocked and can't be trusted"` (invariant across builds, referenced by exactly one ADRP+ADD — note the bare `"Orange State"` has 2 refs and is ambiguous), resolve that pair with `Arm64FindAdrpAddTargeting`, then walk backward (≤0x40) to the nearest `CBZ Wn` and `RewriteBUncond` it (target preserved). The intervening instructions between CBZ and ADRP are X-form `CBZ/CBNZ`, stepped over by the W-form match; the guard is consistently ADRP-0x1C. Idempotency: after rewrite the guard slot is a forward unconditional `B`, which the backward scan recognizes as already-applied (PATCH_OK). No instruction-pattern fallback — the string anchor is strictly more robust, and a build divergent enough to break it would also break a byte pattern. Cross-build gate `tests/host/088_patch7_multi_abl.sh`: infiniti-EU-16.0.5.703 (CBZ @0x78F0), infiniti-IN-16.0.7.201 (@0x76D8), op15t-fairlady-201 (@0x76D8), fairlady-CN-16.0.7.200 (@0x76D8) all → OK + idempotent; non-oplus (xi17-pudding Xiaomi, myron) correctly MISS. Ghidra: bookmark+comment on the 201 CBZ @0x76D8 in the `gbl_root_canoe` project (`abl-201.efi`); the warning block it skips loads "Orange State" at +0x14 via ADRP@0x76EC.
- `patch1` efisp-recursion guard MISS on IN-16.0.7.201 is **expected, not a regression**. The guard only exists where the ABL contains the EFISP re-entry marker; `fv-unwrap` reports `efisp-marker: present` on EU-16.0.5.703 but `efisp-marker: absent` on IN-16.0.7.201, so there is nothing for patch1 to anchor on in 201. A staged `oem boot-efi` one-shot does not persist an ABL to EFISP, so the missing guard is harmless on the dev loop. (patch1's anchor is itself EU-specific; re-deriving it for other builds is a separate follow-up, tracked apart from patch7.)
- Fixture coverage lives under `tests/images/`; keep the fixture README as the live map for which raw FV wrappers or extracted PEs each patch test consumes.

## QSEE / KM / SPSS follow-ups

- QSEE/KM call-slot mapping and SPSS/Secretkeeper follow-ups were investigated enough to guide mode-2 planning, but mode-2 still needs profile lifecycle work before it becomes a usable mode.
- Preserve future notes as profile evidence: command IDs, payload shapes, and OTA/profile coupling should be stored here instead of in session transcripts.

## Device: macan / sm8845 (OnePlus 15R ≈ OnePlus Ace 6T)

Source: mode-0 diag bundle `gbl-chainload-diag-20260604-000513` (slot a active, recovery boot). Identity is from hard partition/firmware evidence, not recovery props:

- ABL string `OPLUS SM8845 Attestation`; `ro.board.platform=sm8845`; `ro.product.device=macan`; `ro.product.model=OnePlus 15R`; SoC internal `Molokai`/`Kaanapali`; OEM ID `0x51`. (The phone was labelled "Ace 6T / canoe / sm8850" — the bundle is unambiguously **macan / sm8845**; Ace 6T is the CN marketing name for the macan device, canoe/sm8850 is a different SoC.)
- The build still bakes `-DPRODUCT_NAME="canoe"` (dev-project label, cosmetic getvar string). Not load-bearing; left as-is.

### KM device-state RPMB persistence — mode-1 brick path (FIXED)

macan issues an **OEM-added** KM command `WRITE_KM_DEVICE_STATE` (cmd `0x203`, `KEYMASTER_UTILS_CMD_ID + 3`) that is **absent from the open QcomModulePkg BSP** (only the enum exists; no caller). The BSP `KeyMasterSetRotAndBootState` sends only SET_ROT (`0x201`) + SET_BOOT_STATE (`0x208`) and never calls `0x203`, so the RoT/boot-state sends do **not** themselves persist.

**Evidence basis (read this before trusting the mechanism).** The primary capture is a *mode-0* bundle, so mode-1 behavior on macan is **not directly observed**. The fix instead rests on a controlled comparison with the project's own *validated* mode-1 capture on infiniti (`logs/20260524-012314_manual_mode-1-v2.3.4-installed`):

| KM cmd | infiniti mode-1 (validated, no brick) | macan mode-0 |
| --- | --- | --- |
| SET_ROT `0x201` | 7× | 1× |
| SET_BOOT_STATE `0x208` | 7×, **isUnlocked=0** | 1×, isUnlocked=1 |
| **WRITE_KM_DEVICE_STATE `0x203`** | **0×** | **2×** |

Infiniti's validated mode-1 already drives a fully *locked* RoT + boot-state (`isUnlocked=0`, real pubkey) into the KM TA every boot and does **not** brick — because the locked state is ephemeral RAM attestation that is never persisted (infiniti's ABL lacks `0x203` entirely). macan adds exactly that one persistence command. Suppressing `0x203` under fakelock therefore makes macan mode-1 converge onto the KM behavior infiniti is validated safe with, and is safe to suppress *because* infiniti's KM TA works with `0x203` never issued.

mode-0 baseline on macan is honest ORANGE/unlocked: SET_ROT digest = `4bf5122f…` = `SHA256(0x01)` = `SHA256(IsUnlocked=1)` (matches BSP ORANGE branch); SET_BOOT_STATE `isUnlocked=1`, `pubKey=0…0`. Under mode-1 the ABL is expected (by analogy to infiniti) to flip to GREEN/YELLOW and drive a locked RoT/boot-state, which macan's `0x203` would then commit to RPMB — the reported "mode-1 locked status propagated to RPMB" brick.

**Caveat — NOT proven:** (a) no macan mode-1 capture exists, so macan's mode-1 SET_ROT/SET_BOOT_STATE + `0x203` payload is inferred, not observed; (b) `0x203` → RPMB is inferred from the command name + the user report + the infiniti control, not confirmed by disassembling its handler; (c) the `VB: RWDeviceState: Succeed using rpmb!` lines are the **VB DeviceInfo** path (they appear throughout the infiniti mode-1 log with zero `0x203`), **not** evidence that `0x203` writes RPMB — an earlier draft wrongly cited that bracketing. Definitive confirmation needs a macan mode-1 `--debug --verbose` staged capture.

The pre-existing fakelock suppressed three RPMB lock-state paths (VB `WRITE_CONFIG` swallow, VB reset swallow, OplusSec `0x0A write_rpmb_boot_info` drop); `0x203` is the suspected unguarded **fourth**, present on macan and absent on infiniti. Fix: `FakelockOverlay_ShouldDropKmDeviceStateWrite` swallows `0x203` (return EFI_SUCCESS, no forward) under `WantFakelockHook`, gated to the keymaster TA handle in `QseecomHook`. The current-boot attestation context (set via `0x201`/`0x208`, RAM-only) is unaffected — only the `0x203` commit is dropped, so fakelock still *reports* locked while never *persisting* it. Universal under fakelock (correct on any device; a no-op where `0x203` isn't issued), so no per-SoC gate was added.

keymaster-handle attribution: keymaster is loaded by `LoadSecureApps` (AppId `0xFFFF0001`) before our QseecomStartApp hook, so the first `0x203` we intercept precedes the StartApp tag. `QseecomHook` pins `gKeymasterHandle` lazily from the first cmd in the `0x200–0x2FF` `KEYMASTER_UTILS` space (no other hooked TA uses that range), which lands in time to gate that first write; the StartApp `"keymaster"` tag is a redundant confirmation.

### SPSS absent on sm8845 — expected, not a bug (FIXED handling)

`SpssHook: LocateProtocol(gEfiSPSSProtocolGuid) → Not Found` on macan, and the SPSS subsystem itself fails to come up earlier (`SPSSLib_LoadSPSS … PMIC clients failed: 0xE`). This is the BSP's own documented path: `ShareKeyMintInfoWithSPU` (QcomModulePkg `KeymasterClient.c`) treats `EFI_NOT_FOUND` as *"this chipset doesn't have support for sharing keymint info"* and returns success — macan does **not** mirror KeyMaster RoT/BootState/Vbh to an SPU, so there is **no SPU enforcement domain to spoof**. The KeyMaster/QSEECOM overlay (`ProfileOverlay_RewriteKmSend`) is authoritative for both fakelock and profile-spoof on this SoC.

Bug this exposed: `InstallAll` treated SPSS install failure as **FATAL** whenever `WantProfileSpoof` was set, so mode-2 on macan would have aborted the chain-load at LocateProtocol. Fix: `InstallSpssHook` returns `EFI_NOT_FOUND` with a benign `GBL_INFO`, and `InstallAll` special-cases `EFI_NOT_FOUND` (expected=0 slots, no abort). A *real* SPSS failure on a chipset that **does** publish the protocol (canoe/infiniti-class) stays fatal under profile-spoof — that mirror must be hooked there.

## LogFs and logging

- Earlier LogFs mount failures came from staged EFI ordering and `ConnectController` assumptions.
- The durable fix pattern is probe-first `SimpleFileSystem`, tolerate `EFI_ALREADY_STARTED` / `EFI_NOT_FOUND`, and avoid remount churn.
- A second `LogFsInit` on the regular path is a likely avoidable cost center.

Evidence to preserve:

- Staged EFI and FV-loaded primary GBL contexts behave differently; mount code must tolerate both.
- Current minimal logging design removed the private `GblChainload_BootN.txt` mirror and relies on UefiLog rotation/snapshot behavior.

## Boot-time performance

- Largest suspected costs: ABL unwrap/LZMA, LogFs mount/rotation, then `LoadImage` / `StartImage`.
- Likely optimizations: skip duplicate LogFs init, read only wrapped FV bytes, and avoid double-decompression.
- Partition enumeration, FV driver loading, patch scans, and hook installation are second-order until measured otherwise.

## Live fixture note

- Test fixture naming and harness coverage remain documented in `tests/images/README.md`.
