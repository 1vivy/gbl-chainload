# Mode-1 oplusreserve1 write-block — final static prep

Program: `LinuxLoader_infiniti.efi` in Ghidra project `gbl_root_canoe`.
Scope: static validation for mode-1 BlockIo write-swallow on `oplusreserve1`. No device experiments.

Related notes:

- `2026-05-13-fastboot-gate-static-close.md`
- `2026-05-13-abl-unlockrecord.md`
- `2026-05-13-fastboot-loss-state.md`
- `2026-05-13-oplussec-handover.md`

## Applied Ghidra annotations in this pass

Renamed / added comments:

```text
0x09474 BuildAndroidBootConfigAndCmdline
0x1f238 EnableUartFlagInReserve1
0x1f2f0 ClearUartFlagInReserve1
0x52c94 PersistDdrBootControlMarkComplete
0x52cf8 AppendAndPersistDdrBootControlEvent
0x5303c LoadDdrBootControlFromReserve1
0x54c9c RunUefiDdrTestAndPersistControl
0x52e34 ReadWriteDdrReserveTail   (plate comment updated)
```

Added decompiler/disassembly comments at the three unlock-sensitive write sites:

```text
0x328d0  HandleBootMenuSelection token wipe WriteBlocks(LastBlock-0x3a5)
0x35114  UpdateUnlockRecord WriteBlocks(LastBlock-0x35c)
0x08a7c  LinuxLoaderEntry UnlockRecord init WriteBlocks(LastBlock-0x35c)
```

## T-W1: complete writer table

Static method used:

- Searched string/xrefs for `oplusreserve1` and alias `opporeserve1`.
- Enumerated callers of `ReadWritePartitionBlocks` (`0x338a0`) and checked write flag `w4 != 0`.
- Enumerated direct BlockIo handle users that call method `+0x20` (`WriteBlocks`) after locating `oplusreserve1`/`opporeserve1`.
- Enumerated `ReadWriteDdrReserveTail` callers, since it wraps a tail-region `WriteBlocks` for `oplusreserve1`.

### Complete observed `oplusreserve1` / `opporeserve1` writers

| Site | Function/address | Target LBA | Bytes written | Trigger / path | Return handling |
|---|---:|---|---|---|---|
| Token wipe | `HandleBootMenuSelection` `0x324b0`, write at `0x328d0` | `LastBlock - 0x3a5`; in 8 MiB images LBA `1114` / offset `0x45a000` | zeroed 0x1000 stack buffer | User-confirmed normal bootloader-lock warning UI, menu state `8`, action id `2` | Nonzero logs `Write fastboot_unlock_data WriteBlocks failed :%r`; then continues to `UpdateUnlockRecord`. |
| UnlockRecord update/reseal | `UpdateUnlockRecord` `0x34cd0`, write at `0x35114` | `LastBlock - 0x35c`; in 8 MiB images LBA `1187` / offset `0x4a3000` | 0x1000 stack block with hash-sealed UnlockRecord at start | Lock/unlock state transitions via `SetDeviceUnlockStateWithUi` / `HandleBootMenuSelection`; also verify/reseal mode | Nonzero logs `%a: Write UnlockRecord WriteBlocks failed\r\n` and returns the error to caller. |
| UnlockRecord initializer | `LinuxLoaderEntry` `0x4e50`, build/hash around `0x7b64`/`0x8260`, write at `0x08a7c` | `LastBlock - 0x35c`; LBA `1187` in sample images | newly initialized 0x1000 block: magic, serial-hash16, counter/status, in-band SHA-256 | Boot-time only if existing UnlockRecord is missing/hash-mismatched/uninitialized | Nonzero logs and rejoins normal `LinuxLoaderEntry` continuation; no immediate halt/reboot solely from this write failure. |
| UART enable flag | `EnableUartFlagInReserve1` `0x1f238` | absolute LBA `0x429` | existing block after copying `ENABLE_UART:TRUE` into first 0x10 bytes | Data-driven command/table entry at `0x7b348`; debug/UART enable action | Nonzero calls UI/error helper `FUN_00019d94`, then returns through normal UI cleanup. |
| UART clear flag | `ClearUartFlagInReserve1` `0x1f2f0` | absolute LBA `0x429` | zeroed 0x1000 stack block | Data-driven command/table entry at `0x7b358`; debug/UART disable action | Nonzero calls UI/error helper `FUN_00019d94`, then returns through normal UI cleanup. |
| `oplus_charge` marker | `BuildAndroidBootConfigAndCmdline` `0x09474`, write call at `0x0b3f0` | absolute LBA `0x233e` or `0x48a` depending storage path (`FUN_000339c4`) | boot/cmdline block after writing `oplus_charge` and clearing `allow_kpoc` field | Android bootconfig/cmdline build from `BootLinux_Qcom`; fires during normal Linux boot path | Nonzero logs `abl oplusreserve1 write error`; continues bootconfig construction. |
| DDR boot-control tail | `ReadWriteDdrReserveTail` `0x52e34` | byte offset `partition_size - 0x20000`, converted to block LBA. For an 8 MiB / 4K-block image this is byte `0x7e0000`, LBA `2016`. | global DDR control/log buffer (`DAT_000bad38`, related globals) | DDR/UEFI test and screen-test/control paths: `PersistDdrBootControlMarkComplete`, `AppendAndPersistDdrBootControlEvent`, `RunUefiDdrTestAndPersistControl`; `LoadDdrBootControlFromReserve1` is read side | Nonzero logs `DS_ReadWritePartiton failed :%r` and returns status to DDR-test caller. |

### Checked non-writers / read-only reserve users

These reference `oplusreserve1` but do not write to it in this fixture:

| Function | Behavior |
|---|---|
| `FastbootUnlockVerify` `0x3e440` | Reads token block at `LastBlock-0x3a5`; no write. |
| `GetOplusSerialFromReserve1` `0x33b50` | Reads Oplus serial blob at `0x26fa`/`0x520`; no write. |
| `FUN_00033040` | Reads OCDT/export whitelist-ish partition data via `ReadWritePartitionBlocks(..., write_flag=0)`; no reserve write. |
| `FUN_00054824` | Reads a reserve block (`0x2386`/`0x4a5`) to initialize globals; no write. |
| `ReadReserve1TailBlock`, `IsUartEnabledFromReserve1`, `FUN_0004c264`, `FUN_0004c3dc` | Tail-relative read helpers / consumers; no write. |
| `UpdateDeviceTree_Qcom` | Has `oplusreserve1` xref but observed path is read/boot data use, not a writer. |

### T-W1 conclusions

- No writer to LBA `1114` / `LastBlock-0x3a5` was found outside `HandleBootMenuSelection` state-8 token wipe.
- No writer to LBA `1187` / `LastBlock-0x35c` was found outside `UpdateUnlockRecord` and the `LinuxLoaderEntry` initializer.
- A boot-time autonomous writer does exist, but it is not unlock-token-relevant: `BuildAndroidBootConfigAndCmdline` writes the `oplus_charge` block at `0x233e`/`0x48a` during normal Linux boot.
- DDR test/control writers are non-unlock and target the tail control region, not LBA 1114 or 1187.

## T-W2: UnlockRecord init failure path

Single-sentence verdict: **If the `LinuxLoaderEntry` UnlockRecord initializer write fails, ABL logs the write failure, then rejoins the ordinary `LinuxLoaderEntry` continuation; it does not immediately reboot, halt, or enter recovery solely because that write failed.**

Supporting trace:

```text
LinuxLoaderEntry
  0x7b64  BuildUnlockRecordHashPayload(...)
  0x8260  BuildUnlockRecordHashPayload(...) after serial/status initialization
  0x8a48  Locate/Open BlockIo handle for oppo/oplusreserve1
  0x8a5c  load BlockIo handle
  0x8a68  load method +0x20 (WriteBlocks)
  0x8a78  x2 = LastBlock - 0x35c
  0x8a7c  blr x10   ; WriteBlocks(..., stack record)
  0x8a80  cbz x0, 0x7a24        ; success rejoins normal flow
  0x8a84  mov x20, x0
  0x8a90  b 0x7b34              ; error log path
  0x7b34..0x7a20 log read/write error strings and status
  0x7a24  call FUN_0000da40(...)
  0x7a2c  branch by pre-existing mode flag back to 0x6d64 or 0x6cdc
```

The failure path eventually reaches the same main boot/fastboot region as the success path. For fastboot entry, the known gate at `0x6d78` still runs normally afterward.

Risk assessment: this initializer should be rare because it only fires when the existing record is missing/mismatched. Even if mode-1 blocks that write, the static failure path is benign from a control-flow standpoint. The practical downside is repeated log noise / repeated attempted repair on later boots until a real write is allowed or reads are virtualized.

## T-W3: WriteBlocks return-value handling

| Site | Write call | Non-success behavior | Swallow return recommendation |
|---|---|---|---|
| State-8 token wipe | `HandleBootMenuSelection:0x328d0` writes `LastBlock-0x3a5` zero block | Checks return. Nonzero logs `Write fastboot_unlock_data WriteBlocks failed :%r`, then continues to `UpdateUnlockRecord` and selected-action exit. No retry/halt observed. | Return `EFI_SUCCESS` (`0`) so UI-confirmed lock path sees a clean token-wipe success while the hook preserves the real block. |
| UnlockRecord update/reseal | `UpdateUnlockRecord:0x35114` writes `LastBlock-0x35c` record | Checks return. Nonzero logs `%a: Write UnlockRecord WriteBlocks failed\r\n` and returns nonzero to caller. Callers then log and continue, but success path is cleaner. | Return `EFI_SUCCESS` (`0`) for swallowed writes. |
| UnlockRecord initializer | `LinuxLoaderEntry:0x08a7c` writes `LastBlock-0x35c` initialized record | Checks return. Nonzero logs and rejoins normal entry continuation; no immediate reboot/halt. | Return `EFI_SUCCESS` (`0`) for swallowed writes to avoid repeated failure handling/logs. |

Other non-unlock writers:

- UART flag and DDR-control writers also check/log nonzero. If a partition-wide swallow is used, returning `EFI_SUCCESS` is the least disruptive behavior for these callers too.
- `BuildAndroidBootConfigAndCmdline` logs on `oplus_charge` write failure and continues, but returning `EFI_SUCCESS` avoids noisy false failures during every Linux boot.

## Hook scope decision

Recommended implementation: **swallow partition-wide `oplusreserve1` writes in mode-1, returning `EFI_SUCCESS`, if the design goal is a full runtime reserve1 preservation overlay while chainloaded.**

Justification:

- The complete static writer set has no surprise writer to token LBA `1114` outside the confirmed lock UI path.
- The complete static writer set has no surprise writer to UnlockRecord LBA `1187` outside known accounting/init paths.
- The additional partition-wide casualties are non-unlock operational breadcrumbs (`oplus_charge`, UART flag, DDR-test control). Static return handling is benign if the hook returns success.
- For mode-1's stated goal — preserve DeepTest token across relock and avoid ABL/Oplus state churn while chainloaded — partition-wide swallow is simpler and safer than a partial allowlist, because it prevents future unidentified reserve1 accounting writes from creating divergence.

Alternative conservative scope: swallow only:

```text
LastBlock - 0x3a5   fastboot_unlock_data / DeepTest token
LastBlock - 0x35c   UnlockRecord
```

This avoids suppressing `oplus_charge`, UART, and DDR-test breadcrumbs, but it requires correctly handling absolute-vs-tail LBA normalization and does not buy much functional safety for normal users. Use this only if preserving non-unlock reserve1 breadcrumbs during mode-1 becomes a requirement.

## Implementation notes for mode-1

1. Return value:
   - Return `EFI_SUCCESS` (`0`) for swallowed `oplusreserve1` writes.
   - Do not propagate the underlying blocked status; callers either log or return errors on nonzero.

2. Matching:
   - Match the partition by resolved BlockIo handle / partition label, not by raw LBA alone.
   - Include both aliases where relevant: `opporeserve1` and `oplusreserve1`.
   - LBA values in the compared reserve images are 4K logical blocks, not 512-byte sectors.

3. Preserve-vector priority if implementing an allowlist instead of partition-wide swallow:
   - `LastBlock - 0x3a5`: must preserve real token block.
   - `LastBlock - 0x35c`: should either swallow writes and/or provide a coherent virtual locked-looking read if the physical record is invalid.

4. Boot behavior edge cases:
   - If an existing UnlockRecord is invalid, blocking initializer writes is control-flow benign but can cause repeated initialization attempts. Virtualizing a valid locked-looking UnlockRecord read eliminates that loop.
   - Partition-wide swallow may hide UART/debug toggles and DDR-test persistence while mode-1 is active. This is acceptable for shipping mode-1 unless those service features are explicitly needed.

5. Do not conflate state planes:
   - `oplusreserve1` write-block does not block VB/devinfo writes, OplusSec/RPMB `boot_info`, or misc/BCB `--format_data` writes.
   - Those planes remain separate hooks/policy decisions.

## Final answer

The mode-1 write-swallow should return `EFI_SUCCESS` and may be applied partition-wide to `oplusreserve1` for the shipping preservation path. If minimizing side effects becomes more important than simplicity, narrow it to `LastBlock-0x3a5` plus `LastBlock-0x35c`; no other statically discovered writer targets those two LBAs.

## Implementation follow-up: ProtocolHookLib BlockIo hook

Repo implementation added after the static prep:

- New source: `GblChainloadPkg/Library/ProtocolHookLib/BlockIoHook.c`.
- Build integration: `ProtocolHookLib.inf`, `HookCommon.h`, `InstallAll.c`, and `ProtocolHookLib.h`.
- Install behavior:
  - Enumerates handles implementing `gEfiBlockIoProtocolGuid`.
  - Requires `gEfiPartitionRecordGuid` on the same handle so the hook can match by GPT partition label rather than raw LBA.
  - Installs reserve1 hooks in the first pass and fails mode-1 installation if no `oplusreserve1` / `opporeserve1` BlockIo slot is found; non-reserve telemetry hooks are installed only after reserve protection is confirmed.
  - Stores original `ReadBlocks` / `WriteBlocks` and replaces both slots.
  - Matches both `oplusreserve1` and `opporeserve1` case-insensitively with the same prefix/boundary behavior used by reserve lookup code.
- Runtime behavior:
  - Logs compact logfs-only read/write telemetry: partition, LBA, byte count, block count, and status.
  - Swallows all writes to matched reserve1 partitions in every mode and returns `EFI_SUCCESS` without calling the original `WriteBlocks`.
  - Logs token-specific reasons for known LBAs, including `reason=token-zero-write` when `LastBlock-0x3a5` receives an all-zero buffer.
  - Uses a per-hook `HOOK_REENTRY_GUARD` so logfs-induced BlockIo recursion skips nested logging.
- Mode-1 policy:
  - `InstallAll.c` treats BlockIo as required for universal reserve preservation and includes it in `UniversalRequiredOk` plus install summary counts.

Mode-0 relock test plan:

1. Boot/install mode-0, then install stock firmware.
2. Perform the stock bootloader relock procedure while mode-0 is active.
3. Unlock again and pull logfs.
4. Confirm `blockio | op=write-swallow | reason=token-zero-write | p=oplusreserve1` (or `opporeserve1`) is present. The same event also emits vital user-visible output in every build: `GBL: intercepted reserve token zeroing on oplusreserve1 LBA 1114; token preserved`.

Validation:

- `./scripts/build.sh --mode 1 --verbose` succeeded; produced `dist/mode-1-verbose.efi` (`606208` bytes).
- `./scripts/build.sh --mode 1` succeeded; produced `dist/mode-1.efi` (`606208` bytes).
- `./scripts/build.sh --mode 0` succeeded; produced `dist/mode-0.efi` (`552960` bytes).
- Code-review fixes applied before final build: ASCII partition names use `%a` in DEBUG format strings, and mode-1 install now fails if reserve1 was not actually hooked.
- Note: do not run repo build scripts concurrently; they share `Build/` and a parallel mode-0/mode-1 run can race generated build state.

Confidence: High for compile/build integration. Device-side behavior still needs RAM-load validation only (`fastboot stage dist/<artifact>.efi`; `fastboot oem boot-efi`) per project safety rules.
