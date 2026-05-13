# Fastboot gate static close — 2026-05-13

Program: `LinuxLoader_infiniti.efi` in Ghidra project `gbl_root_canoe`.
Scope: Close T-A/T-B/T-C for OPPO fastboot loss / preserve-vector analysis. No device writes performed.

Related notes:

- `2026-05-13-abl-unlockrecord.md`
- `2026-05-13-fastboot-loss-state.md`
- `2026-05-13-oplussec-handover.md`

## Applied Ghidra annotations in this pass

Renamed / created:

```text
0x28c14 ReadSecurityState
0x28cfc IsSecureBootOrRpmbStateActive
0x28dbc IsDeviceSecureTypeProduction
0x32074 CreateUnlockMenuKeyDetectEvent
0x32154 UnlockMenuKeyDetectCallback
0x3242c SetUnlockMenuKeyDetectTimer
0x32d34 AdjustBootMenuSelectionTowardLowerIndex
0x32df0 AdjustBootMenuSelectionTowardHigherIndex
0x7cce0 BootMenuKeyDispatchTable
0x71088 BootMenuStateToUnlockStateTable
```

Added plate/decompiler comments to `ReadSecurityState`, `SetDeviceUnlockStateWithUi`, `HandleBootMenuSelection`, `UnlockMenuKeyDetectCallback`, and security-state bit tests in `VerifyUnlockToken` / `FastbootUnlockVerify`.

## T-A — persistent writes from `fastboot flashing lock`

Entry points:

```text
0x1e38c FastbootFlashingLock           -> SetDeviceUnlockStateWithUi(0, 0)
0x1e3ec FastbootFlashingLockCritical   -> SetDeviceUnlockStateWithUi(1, 0)
0x1a9a4 FastbootFlashingUnlock         -> SetDeviceUnlockStateWithUi(0, 1)
0x1e3bc FastbootFlashingUnlockCritical -> SetDeviceUnlockStateWithUi(1, 1)
```

Direct/no-display normal lock path:

```text
FastbootFlashingLock
  -> SetDeviceUnlockStateWithUi(0,0)
    -> SetVerifiedBootUnlockState(0,0)
      -> SetDeviceUnlockedFlag(0)
        -> WriteVerifiedBootDeviceInfo / VB protocol method +0x08
      -> ResetVerifiedBootDeviceState / VB protocol method +0x30
      -> misc/BCB write of "recovery\n--format_data\n" via FUN_00018870
    -> UpdateUnlockRecord(0)
      -> write oplusreserve1 UnlockRecord at LastBlock - 0x35c
```

Important nuance: `UpdateUnlockRecord(0)` is verify/reseal/locked-status mode. It sets status `0`; it does not increment the counter unless it first repairs a hash mismatch by resetting counter to `1`.

Display-confirmed normal lock path:

```text
SetDeviceUnlockStateWithUi(0,0)
  -> uses lock warning UI blob at 0x7d8d0
  -> seeds menu state DAT_000bb648 = 8
  -> CreateUnlockMenuKeyDetectEvent(...)
  -> UnlockMenuKeyDetectCallback(...)
  -> HandleBootMenuSelection(context with state 8)
     case action 2:
       SetVerifiedBootUnlockState(table[8].domain, table[8].state)
       zero 0x1000-byte buffer
       WriteBlocks(opporeserve1/oplusreserve1, LastBlock - 0x3a5, zero buffer)
       UpdateUnlockRecord(table[8].state)
```

Conclusion: static analysis shows **two lock shapes**:

1. Direct/no-display `fastboot flashing lock` changes VB/devinfo, requests userdata format via misc/BCB, and writes only UnlockRecord in `oplusreserve1` among known unlock reserve blocks.
2. Display/UI-confirmed normal lock reaches menu state `8` and explicitly zeroes the DeepTest/fastboot token block at `LastBlock - 0x3a5` before `UpdateUnlockRecord(0)`.

Therefore prior wording “lock does not touch token block” was too broad. It is true for direct/no-display path, but false for the warning-menu confirmation path.

## T-B — Gate 0 source and bit semantics

`ReadSecurityState` (`0x28c14`) is the Gate 0 source. It does not read OplusSec `boot_info`. It:

1. Locates SCM protocol GUID at `DAT_00081b8c`.
2. Calls protocol method `+0x38` with SMC ID `0x2000604`.
3. Expects output status `*(sp+0x10) == 1`.
4. Returns `*(sp+0x18)` as a 32-bit security-state word, or `0xffffffff` on failure.

Observed bit semantics from xrefs:

```text
bit 0 == 1  => secureboot is NOT enabled; FastbootUnlockVerify bypasses token/OCDT gate.
bit 0 == 0  => secureboot enabled; normal fastboot authorization continues.

bit 5 == 1  => RPMB is NOT enabled; VerifyUnlockToken skips RPMB model compare.
bit 5 == 0  => RPMB enabled; VerifyUnlockToken reads OplusSec cmd9 boot_info and compares boot_info+0x20 model to token+0x12c.
```

Additional helper tests:

- `IsSecureBootOrRpmbStateActive` uses masks `0x47` or `0x67` depending platform mode and treats `(state & mask) == 0x40` as secure/production-ish.
- `IsDeviceSecureTypeProduction` uses `(state & 0x47) == 0x40`.

Confidence: high for bits 0 and 5 because strings and branches match direct callers; medium for naming of the aggregate `0x40` production/security bit until more callers are typed.

## T-C — selector 8 and menu/key dispatcher

`SetDeviceUnlockStateWithUi(param_1,param_2)` maps unlock requests to warning UI states:

```text
param_1=0, param_2=0  normal lock      -> menu state 8
param_1=1, param_2=0  critical lock    -> menu state 9
param_1=0, param_2=1  normal unlock    -> menu state 5
param_1=1, param_2=1  critical unlock  -> menu state 7
```

UI blobs:

```text
0x7d8d0 lock warning:   "If you lock the bootloader, you will not be able to install custom operating system..."
0x7cde0 unlock warning: "By unlocking the bootloader, you will be able to install custom operating system..."
```

Dispatcher:

- `CreateUnlockMenuKeyDetectEvent` creates an EFI timer/input event using `UnlockMenuKeyDetectCallback`.
- `UnlockMenuKeyDetectCallback` reads `EFI_SIMPLE_TEXT_INPUT_EX` keystrokes, debounces, and dispatches through `BootMenuKeyDispatchTable` at `0x7cce0`.
- Table entries are three handler pointers per menu state: select handler (`HandleBootMenuSelection`), lower-index selection adjustment (`AdjustBootMenuSelectionTowardLowerIndex`), and higher-index adjustment (`AdjustBootMenuSelectionTowardHigherIndex`).
- For state `8`, the select handler is `HandleBootMenuSelection`.

Token-zero trigger:

```text
HandleBootMenuSelection
  action id 2 from selected UI item
  current context state == 8
    => write zeroed 4K block to opporeserve1/oplusreserve1 LastBlock-0x3a5
```

Interpretation: selector/state `8` is not an autonomous boot-time wipe by itself. It is the normal-lock warning UI state, reached by `SetDeviceUnlockStateWithUi(0,0)`, and the token wipe happens only when the key/menu selection confirms the relevant action.

## Revised hypothesis-resolution table

| Hypothesis | Current status | Evidence |
|---|---|---|
| H1/H5 token block invalidated by lock/one-shot consumption | Strong/static-supported for UI-confirmed normal lock | `HandleBootMenuSelection` state `8` writes zero block to `LastBlock-0x3a5`. |
| H2 OplusSec model drift blocks token | Secondary/open | ABL cmd10 path preserves `boot_info+0x20`; model compare only occurs when RPMB enabled (`security bit5 == 0`). |
| H3 local OTA/version binding check | Weak/refuted for this ABL | `VerifyUnlockToken` does not compare binding31 to local properties; it is only RSA-covered and copied to `DAT_000bae78`. |
| H4 UnlockRecord itself gates fastboot | Weak | Fastboot gate consumes security state, ud/engineering/OCDT/token. UnlockRecord has init/update consumers but no fastboot-gate consumer found. |
| H6 direct `fastboot flashing lock` always zeros token | Refined | Direct/no-display path does not; display-confirmed normal-lock menu path does. |

## Preserve-vector specification

Runtime hook / preservation priorities while chainloaded:

1. Preserve or virtualize `oplusreserve1` token block:
   - 4K LBA `LastBlock - 0x3a5`.
   - In compared 8 MiB images: LBA `1114`, byte offset `0x45a000`.
   - Leading candidate for CN post-relock fastboot loss because UI-confirmed lock statically zeroes this block.

2. Virtualize coherent locked-looking UnlockRecord:
   - 4K LBA `LastBlock - 0x35c`.
   - In compared images: LBA `1187`, byte offset `0x4a3000`.
   - Do not return all zeros. Use a valid record:

```text
record+0x00 magic  = 0x939c978a
record+0x04 hash   = SHA256(payload below)
record+0x24 u32    = 1
record+0x28[16]    = SHA256(board_serial_8_ascii)[0..15]
record+0x38 u32    = 1 or preserved sane counter
record+0x3c u32    = 0 locked-looking status

payload = record+0x24..0x27 || record+0x28..0x37 || LE32(counter) || LE32(status) || LE32(0xfe1956c9)
```

3. Do not broadly swallow all `oplusreserve1` writes unless intentionally implementing a full overlay. Known unrelated reserve users include UART/debug flag, `oplus_charge`, DDR/log control, and Oplus serial blob.

4. VB/devinfo and misc/BCB writes are a separate plane from `oplusreserve1`:
   - VB protocol write/reset occurs during lock.
   - misc/BCB gets `recovery\n--format_data\n` after state change.
   - Preserve-vector for DeepTest token does not automatically preserve userdata-format intent or VB state.

## Optional user experiments

Read-only / dump-first experiments that would most directly validate this close-out:

1. On a CN device, dump `oplusreserve1` before and after confirming `fastboot flashing lock` warning UI.
   - Check 4K LBA `LastBlock-0x3a5` / offset `0x45a000` for zeroing.
   - Check LBA `LastBlock-0x35c` / offset `0x4a3000` for UnlockRecord status/counter/hash.

2. Instrument qsee-buf logging to capture full OplusSec cmd9/cmd10 `0x404` buffers.
   - Confirm `boot_info+0x20` model remains stable across lock/unlock states.

3. If testing gbl-chainload preservation, log writes to exact `oplusreserve1` LBAs before deciding whether to swallow/virtualize:
   - `LastBlock-0x3a5` token block.
   - `LastBlock-0x35c` UnlockRecord.

## Final interpretation

The strongest static explanation for CN fastboot loss after relock is not OTA binding or UnlockRecord gating. It is token-block destruction from the normal-lock warning UI path. A preserve strategy should prioritize the DeepTest token block at `LastBlock-0x3a5` and present a coherent locked-looking UnlockRecord at `LastBlock-0x35c` if hiding chainload state, while treating VB/devinfo and misc/format-data as separate state planes.
