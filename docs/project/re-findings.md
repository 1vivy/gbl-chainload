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
- Fixture coverage lives under `tests/images/`; keep the fixture README as the live map for which raw FV wrappers or extracted PEs each patch test consumes.

## QSEE / KM / SPSS follow-ups

- QSEE/KM call-slot mapping and SPSS/Secretkeeper follow-ups were investigated enough to guide mode-2 planning, but mode-2 still needs profile lifecycle work before it becomes a usable mode.
- Preserve future notes as profile evidence: command IDs, payload shapes, and OTA/profile coupling should be stored here instead of in session transcripts.

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
