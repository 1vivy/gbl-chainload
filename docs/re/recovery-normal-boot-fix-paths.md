# Recovery Normal-Boot Fix Paths

**Status (2026-05-13):** Confirmed. With a custom recovery flashed, mode-1's normal-boot path does not complete — the device only completes the recovery-boot path. Nothing pre-empts a working Android-system boot; Android-system simply never gets there. The fix is to re-shape the on-disk recovery image so its in-partition AVB footer matches what the chain reads at boot. Both deliverables are Phase-2 work; nothing in this PR ships the fix itself — this doc just locks in what we know.

## Failure mode

Under mode-1, ABL sees a locked + green verifiedboot view (our `QCOM_VERIFIEDBOOT_PROTOCOL` mutation), emits the stock `androidboot.vbmeta.digest`, and hands off cleanly. AOSP `first_stage_init` then runs the userspace AVB chain (`AvbHandle::Open` → `avb_slot_verify`). On a locked device any non-OK `AvbSlotVerifyResult` is treated as a boot failure, and init routes the device to recovery.

What trips the gate is narrower than the call graph suggests. `init_boot` and `boot` are routinely patched in the wild — root solutions, kernel swaps — without breaking normal boot, because those tools rewrite partition contents while preserving the in-partition AVB footer; the descriptor still validates against the on-disk image even though the binary content changed. Custom recovery is the opposite case: porting a recovery tree wholesale replaces the partition, footer included, so what the chain reads at boot has no valid AVB metadata. **TBD on the exact gate** — whether the userspace AVB walk itself rejects, or dm-verity, or a recovery-specific check fires first. The Phase-2 disk-side graft doesn't depend on the answer; re-installing the stock recovery's AVB footer onto the custom image makes the descriptor satisfiable again whichever gate is firing.

## Fix paths (Phase 2)

Two deliverables, same technique, different delivery surface:

**Host-side script** — `scripts/graft-vbmeta-from-stock.py`. Takes the user's custom recovery image, splices the stock recovery's vbmeta+footer bytes onto it at `round_up(custom_image_size, 4 KiB)`, and writes a patched image the user flashes. With `patch10` in the boot path, ABL emits `verify_result_local=OK` for recovery and patch10 catches the slot-level recoverable error. Technique validated on infiniti during the abandoned `feature/synthesize-fastboot-cmd` exploration (commit `b26686e`, kept on origin as orphan history). The script is rebuilt cleanly under Phase 2 — not salvaged from that branch.

**Device-side module** — same graft, run on-device against the on-device stock vbmeta, automatically, post-recovery-flash. Removes the requirement that the user keep a fastboot host around for every custom-recovery re-flash. Lives in the Phase-2 module suite next to the OTA-cached-ABL module.

The on-disk patch approach is well-trodden in the broader Android modding space — stock boot/init_boot images get patched routinely for root solutions, kernel swaps, etc. Recovery isn't usually patched this way only because devices are typically ported into a recovery tree which is a full porting workload; for our use case we're not porting, we're grafting one descriptor.

## Alternatives that don't fit

The 2026-05-10 investigation listed several mitigations. None of them survive a closer look:

- **Userspace libavb read-handler facade.** Would override `FsManagerAvbOps::ReadFromPartition()` so AVB sees stock content. Not viable from the bootloader stage — we don't have a hook into userspace. The only path to apply this technique would be an untouched-ABL binary-patch workflow that injects a userspace shim, which is much more invasive than the disk-side graft.
- **Mode-2 TZ payload spoof.** Targets attestation (Play Integrity / KeyMaster / VTS keys), not the normal-boot AVB walk. Different problem domain. Custom ROMs that need this re-spoof use userspace solutions (e.g. SusFS4KSU) for hiding; mode-2's design is for getting valid attestation keys, which is orthogonal to whether the device boots.
- **ABL cmdline rewrite (`androidboot.vbmeta.recovery.digest=<custom>`).** The `vbmeta.recovery.digest` key only appears in bootconfig/cmdline on the recovery-boot path; ABL doesn't emit it on the normal-boot path. The exact reason for this conditional emission is on the ABL side — would need to grep `edk2/QcomModulePkg/` to confirm. Either way, writing the value during normal boot doesn't help because Android never sees it where it isn't emitted.
- **Userspace fstab `no_fail` for recovery.** libfs_avb is conditional on verifiedboot state, and locked-state (which we fake) runs the AVB walk regardless of fstab markers. The flag doesn't reach the gate that's firing.

## AOSP source references

Paths are relative to the AOSP tree. Grep against your local checkout or upstream.

- `system/core/init/first_stage_mount.cpp:809–821` — `InitAvbHandle()` calls `AvbHandle::Open()`.
- `system/core/fs_mgr/libfs_avb/fs_avb.cpp:453–535` — `AvbHandle::Open()` and the cmdline-digest re-check.
- `system/core/fs_mgr/libfs_avb/avb_ops.cpp:122–143` — `FsManagerAvbOps` constructor; no-op rollback / pubkey checks (ABL did those).
- `external/avb/libavb/avb_slot_verify.c:276–449` — hash descriptor load + compare; the `avb_safe_memcmp` mismatch path returns `AVB_SLOT_VERIFY_RESULT_ERROR_VERIFICATION`.

## Status of this document

This is the consolidated decision record. The 2026-05-10 investigation that produced it has been folded into the narrative above; the conclusions hold even where some of the specific mechanism claims need more on-device verification. Live work moves to the Phase-2 spec when it begins; nothing here is open in shim scope.
