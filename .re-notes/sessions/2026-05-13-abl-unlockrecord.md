# ABL UnlockRecord analysis — 2026-05-13

Program: `LinuxLoader_infiniti.efi` in Ghidra project `gbl_root_canoe`.
Architecture: AArch64 UEFI image, image base `0x0`.

## Applied Ghidra names/comments

- `0x34cd0` → `UpdateUnlockRecord`
  - Reads `oplusreserve1` LBA `0x2381` on eMMC or `0x4a3` on UFS.
  - Verifies/reseals record hash, updates counter/status, then writes to `LastBlock - 0x35c`.
  - `param_1 == 0`: verify/reseal mode. If hash mismatch, logs `hash data wrong`, resets counter to `1`, sets status `0`, then reseals/writes.
  - `param_1 != 0`: update mode. Increments counter at record `+0x38`, writes status at `+0x3c` to `param_1`, reseals/writes.
  - Always attempts write-back after successful read/crypto path.

- `0x347a0` → `BuildUnlockRecordHashPayload`
  - Builds the exact 32-byte SHA-256 input for the record hash.
  - Output format: `{ uint32 len = 0x20; uint8 data[32]; }`.
  - Hash payload layout:

    ```text
    payload+0x00..0x03 = record+0x24..0x27
    payload+0x04..0x13 = record+0x28..0x37
    payload+0x14..0x17 = LE32(record+0x38 counter)
    payload+0x18..0x1b = LE32(record+0x3c status)
    payload+0x1c..0x1f = LE32(0xfe1956c9)
    ```

  - Therefore the counter and status are inside the hash, but only protected by unkeyed SHA-256 stored in-band at record `+0x04`.
  - Header/magic at record `+0x00` and trailing bytes after `+0x40` are not part of this hash payload.

- `0x1fce4` → `SetDeviceUnlockStateWithUi`
  - Calls device-state update helper `FUN_00027dd4(param_1,param_2)`, then `UpdateUnlockRecord(param_2)`.
  - Display-enabled path shows a UI/menu before applying state.

- `0x324b0` → `HandleBootMenuSelection`
  - On menu case `2`, calls `FUN_00027dd4(...)`, may zero the fastboot token at `opporeserve1/oplusreserve1 LastBlock-0x3a5` when selecting state `8`, then calls `UpdateUnlockRecord(bVar2)`.

## Additional consumer/initializer in `LinuxLoaderEntry`

`LinuxLoaderEntry` has two calls to `BuildUnlockRecordHashPayload` around `0x7b64` and `0x8260`.

Observed behavior:

1. Reads `oplusreserve1` at the same storage-type-dependent LBA (`0x2381` eMMC, `0x4a3` UFS).
2. Builds the 32-byte hash payload and compares SHA-256 to record `+0x04`.
3. If hash matches, logs/returns via `"%a: have initialized return\r\n"` and does not rewrite.
4. If missing/mismatched, initializes the record using:
   - board serial from `FUN_0001996c`, left-padded to 8 ASCII chars,
   - SHA-256(serial[0..7]) material copied into the record,
   - current lock state from `DAT_000b13c5` (`"is unlocked"` / `"is locked"` log),
   - then reseals and writes to `LastBlock - 0x35c`.

Confidence: Medium-high. Exact record field names in this initializer still need cleanup, but the control flow and hash/write behavior are clear.

## Reserve1 helper functions and LBA map

Applied names:

- `0x338a0` → `ReadWritePartitionBlocks`
  - Generic absolute-LBA helper: `param_5 == 0` selects BlockIo `ReadBlocks` (method `+0x18`), nonzero selects `WriteBlocks` (`+0x20`).
  - Arguments: `(buffer, start_lba, block_count, partition_name, write_flag)`.

- `0x33710` → `ReadReserve1TailBlock`
  - Tail-relative read helper. Rejects offsets `>= 0x800`.
  - Actual LBA: `LastBlock - 0x7ff + offset`.
  - Therefore offset `0x4a3` maps to `LastBlock - 0x35c`, the UnlockRecord block.
  - Important nuance: the eMMC-looking offset `0x2381` used by `UpdateUnlockRecord`/`LinuxLoaderEntry` would be rejected by this helper in this image (`cmp x1,#0x800; b.cc ...; else return -1`). This may be dead eMMC code, a storage-type inversion, or a decompiler/signature artifact in adjacent storage detection. UFS path is coherent.

- `0x33b50` → `GetOplusSerialFromReserve1`
  - Reads `oplusreserve1` absolute LBA `0x26fa` or `0x520` depending on storage path.
  - Copies/deobfuscates first `0x19` bytes with fixed ASCII material, verifies an RSA signature using `FUN_00034310`, then emits a 16-byte Oplus serial or `0000000000000000` on signature failure.
  - Used by bootconfig/cmdline construction before falling back to platform serial.

- `0x33810` → `IsUartEnabledFromReserve1`
  - Reads tail-relative offset `0x429`, i.e. `LastBlock - 0x3d6`, and checks for `ENABLE_UART:TRUE`.
  - Influences `androidboot.console=ttyMSM0` / cmdline console behavior.

- `0x52e34` → `ReadWriteDdrReserveTail`
  - DDR/control-info reserve helper. For `oplusreserve1`, computes an offset near partition end (`partition_size - 0x20000`) and reads/writes DDR screen-test/log control buffers.
  - Not unlock-state critical, but broad reserve1 write hooks must not accidentally break it unless intentionally virtualized.

Other observed `oplusreserve1` users:

- `FUN_0004c264`: reads tail-relative offset `0x429` (`LastBlock - 0x3d6`) for UART/rate-limit/trace flags.
- `FUN_0004c3dc`: reads tail-relative offset `0x514` (`LastBlock - 0x2eb`) for a `bf:` value.
- `FUN_0001f238`: writes absolute LBA `0x429` after copying `ENABLE_UART:TRUE` into the buffer.
- `FUN_0001f2f0`: writes zeroed buffer to absolute LBA `0x429`.
- `LinuxLoaderEntry`: reads/writes `oplus_charge` marker around absolute LBA `0x233e` or `0x48a`, depending on storage path, while building bootconfig/cmdline.

## VerifiedBoot / devinfo state helpers

Applied names:

- `0x27dd4` → `SetVerifiedBootUnlockState`
  - Top-level setter called only from `SetDeviceUnlockStateWithUi` and `HandleBootMenuSelection`.
  - `param_1 == 0`: normal unlock flag path.
  - `param_1 == 1`: critical-unlock flag path.
  - Calls `ResetVerifiedBootDeviceState` and writes `recovery\n--format_data\n` to misc/BCB via `FUN_00018870` after changing state.

- `0x27f70` → `SetDeviceUnlockedFlag`
  - Updates `DAT_000b13c5` when the requested byte differs, then calls `WriteVerifiedBootDeviceInfo`.

- `0x27fec` → `SetCriticalUnlockedFlag`
  - Updates `DAT_000b13c6` when the requested byte differs, then calls `WriteVerifiedBootDeviceInfo`.

- `0x183e8` → `WriteVerifiedBootDeviceInfo`
  - Locates VerifiedBoot protocol `DAT_0007b4d0` and invokes method `+0x08` with the global device-info buffer length `0xd10`.

- `0x1898c` → `ResetVerifiedBootDeviceState`
  - Locates VerifiedBoot protocol and invokes method `+0x30` when the platform path requires a VB reset.

This matches the current gbl-chainload design assumption: devinfo/VB state is a separate plane from `oplusreserve1`. If RPMB/devinfo writes are swallowed and reads are spoofed locked in mode-1, then the remaining Oplus confusion source is reserve1-side accounting/token/debug state, not canonical VB state.

## Hooking implications refined

Do not broadly swallow every `oplusreserve1` write unless intentionally creating a full virtual reserve1 overlay. There are non-unlock uses (UART/debug flags, DDR test/log/control, `oplus_charge`, boot-mode breadcrumbs).

Unlock-sensitive reserve1 blocks to prioritize:

```text
LastBlock - 0x3a5   fastboot_unlock_data / DeepTest token block
LastBlock - 0x35c   UnlockRecord block (also tail-relative offset 0x4a3)
```

Likely useful virtual-read behavior while gbl-chainload is active:

- For `LastBlock - 0x35c`, return a coherent locked-looking UnlockRecord with valid SHA-256, instead of only swallowing writes.
- For `LastBlock - 0x3a5`, either return a stock/empty token block or the original pre-mod block, depending on desired DeepTest/userspace behavior.
- Return `EFI_SUCCESS` for swallowed writes to those exact blocks so ABL/Oplus callers do not enter error paths.

For uninstall/good-exit, a root-side reconciliation tool is still recommended because runtime write-swallowing cannot repair already-diverged reserve state after the hook is gone.

## `images/oplusreserve1_*` comparison

Inputs compared read-only:

```text
images/oplusreserve1_cn_freshlyunlocked.img
  size 0x800000, SHA256 44b83b50f49fcd560cea26abec5a80b2e1ce17a338375e73d8526c8b1ad467ef

images/oplusreserve1_na.img
  size 0x800000, SHA256 e27d0cf88920f98cef64c5e94774e4f3531d72155dd61c076b020d703f9e93e8
```

Critical correction from the image comparison: the relevant LBAs are **4096-byte logical blocks**, not 512-byte sectors. Each image has 2048 4K blocks, so `LastBlock == 2047`.

Known block map in these images:

```text
LastBlock - 0x3a5 = LBA 1114 = byte offset 0x45a000  fastboot_unlock_data token
LastBlock - 0x35c = LBA 1187 = byte offset 0x4a3000  UnlockRecord
LastBlock - 0x3d6 = LBA 1065 = byte offset 0x429000  ENABLE_UART/debug flag block
LBA 0x520       = byte offset 0x520000              Oplus serial blob
LBA 0x48a       = byte offset 0x48a000              oplus_charge block
```

### Fastboot token block (`0x45a000`, LBA 1114)

- CN freshly DeepTest-unlocked image: populated, 316 nonzero bytes in first `0x140`.
  - Token serial at `+0x100`: `be55ac8a`
  - New-struct marker at `+0x108`: `0002`
  - Permission at `+0x10c`: ASCII `1`
  - Model at `+0x12c`: `PLK110##########`
  - Block SHA256: `f31d0ae6eea0b3b603c392effb35315bb92c47a7b326c4c6ca631e659cda814e`

- NA always-fastboot image: all zero.
  - Block SHA256: `ad7facb2586fc6e966c004d7d1d16b024f5805ff7cb47c7a85dabd8b48892ca7` (SHA-256 of a zeroed 4K block)

Interpretation: the NA fastboot-access path is not token-driven. Returning a zeroed token block is a plausible locked/stock-looking virtual read for devices where DeepTest state should be hidden.

### UnlockRecord block (`0x4a3000`, LBA 1187)

Both images have a valid hash-sealed UnlockRecord with magic `8a979c93`.

CN freshly unlocked:

```text
stored hash: 5e8182dcbcc9a1d8bbe039b1615ee6d297d96d2e287def12977b2d81e5448fdf
record+0x24: 01000000
record+0x28: 5fda1b1d88cc8d6343e34155bf420c6c
record+0x38 counter: 1
record+0x3c status:  1
hash verifies: yes
```

NA always-fastboot:

```text
stored hash: 728c8e44913a67d6f20c6ab544597b7a8f0a8b6d99f99bc97a173f1bcab27910
record+0x24: 01000000
record+0x28: 5f50d769f7ef1a63d43c1e7aa53fd9dc
record+0x38 counter: 1
record+0x3c status:  0
hash verifies: yes
```

`record+0x28..0x37` appears to be the first 16 bytes of `SHA256(board_serial_8_ascii)`:

- For CN, `SHA256("be55ac8a") = 5fda1b1d88cc8d6343e34155bf420c6c...`, matching `record+0x28`.
- This makes the UnlockRecord layout more concrete:

```text
0x00  u32 magic = 0x939c978a (little endian bytes 8a 97 9c 93)
0x04  u8[32] SHA256(BuildUnlockRecordHashPayload)
0x24  u32 version_or_initialized = 1
0x28  u8[16] first half of SHA256(board_serial_8_ascii)
0x38  u32 counter
0x3c  u32 status (CN unlocked=1, NA always-fastboot/locked-looking=0)
```

This strongly supports using `status=0, counter=1, version=1, serial_hash16=current_board_serial_hash16` as the locked-looking reconciliation target, not an all-zero UnlockRecord.

### Oplus serial blob (`0x520000`, LBA 0x520)

Decoded with the deobfuscation used by `GetOplusSerialFromReserve1`:

```text
CN decoded bytes: 48 7b 07 62 65 35 35 61 63 33 42 31 35 41 43 30 30 37 59 48 30 30 30 30 30
CN serial emitted by function (bytes +9..+24): 3B15AC007YH00000

NA decoded bytes: 2c ae 22 66 36 31 34 64 61 33 43 31 35 41 54 30 30 33 5a 42 30 30 30 30 30
NA serial emitted by function (bytes +9..+24): 3C15AT003ZB00000
```

This Oplus serial is distinct from the 8-byte board serial used in the DeepTest token and UnlockRecord hash.

### Broader block differences

There are 56 differing 4K blocks. Many are unrelated operational data. Unlock-relevant blocks among them:

```text
0x45a000 / LBA 1114  token: CN populated, NA zero
0x4a3000 / LBA 1187  UnlockRecord: both valid; CN status=1, NA status=0
0x520000 / LBA 1312  Oplus serial blob differs per device
```

Other notable reserve users/differences:

```text
0x429000 / LBA 1065  ENABLE_UART:FALSE in both
0x48a000 / LBA 1162  oplus_charge data differs; not unlock critical
0x514000 / LBA 1300  bf block zero in both
```

### Refined good-exit target

For a device-specific root-side reconciliation program, do **not** write a stock NA block verbatim to another device. Build a device-specific locked-looking UnlockRecord:

1. Read current 8-byte board serial using the same source as ABL/token path, or accept it as an argument.
2. Set:
   - magic `0x939c978a`
   - version/initialized `record+0x24 = 1`
   - `record+0x28..0x37 = SHA256(serial8)[0..15]`
   - counter `record+0x38 = 1` (or preserve if we learn a better stock value)
   - status `record+0x3c = 0`
3. Recompute in-band SHA-256 using `BuildUnlockRecordHashPayload` rules and store at `record+0x04`.
4. Consider zeroing/virtualizing token block `0x45a000` if the desired post-uninstall state is DeepTest-hidden / locked-looking to Oplus userspace.

For runtime gbl-chainload hooks, a coherent virtual locked-looking UnlockRecord is better than returning zeros; a zeroed UnlockRecord will fail hash validation and cause ABL to initialize/repair/write it.

## Implications

- Resetting the unlock increment is feasible if a writer can modify `oplusreserve1`: set record `+0x38` to the desired counter, set `+0x3c` to desired status, recompute SHA-256 over the 32-byte payload above, and write digest to record `+0x04`.
- Blocking `oplusreserve1` writes would block writes to both:
  - token block at `LastBlock - 0x3a5`, and
  - UnlockRecord at `LastBlock - 0x35c`.
- However, `UpdateUnlockRecord` is not the primary fastboot gate. `fastboot_unlock_verify` still uses secureboot/engineering/OCDT/token checks. The UnlockRecord appears to be a boot/display/state accounting object and transition log, not the RSA unlock verifier itself.
- A protocol hook that blocks only UnlockRecord writes may prevent increment/status recording, but callers can log errors and continue in some paths. It should not be treated as sufficient for a clean “good user exit” until device-state writes (`FUN_00027dd4`, `FUN_000183e8`) are mapped.
- The `prj` whitelist is not an RPMB model-name field. `prj` comes from signed OCDT (`DAT_000b2114`). Changing RPMB model name affects only the new-struct unlock-token model match, not OCDT whitelist status.

## Follow-up

1. Rename/type fields in the UnlockRecord stack buffer in `LinuxLoaderEntry` initializer.
2. Reverse `FUN_00027dd4` and `FUN_000183e8` to identify the canonical persistent device unlock state.
3. If implementing a gbl-chainload hook, target/observe writes to `oplusreserve1` LBAs `LastBlock-0x3a5` and `LastBlock-0x35c`, but also track device-info writes.
4. Confirm whether write failures in `SetDeviceUnlockStateWithUi` and `HandleBootMenuSelection` affect user-visible success/failure or merely logging.
