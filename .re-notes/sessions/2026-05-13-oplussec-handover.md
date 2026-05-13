# Handover: OplusSec / RPMB boot_info payload status

Date: 2026-05-13
Target: returning RE/design agent
Project: `gbl-chainload`, Ghidra project `gbl_root_canoe`, program `LinuxLoader_infiniti.efi`
Related notes: `2026-05-13-fastboot-loss-state.md`, `2026-05-13-abl-unlockrecord.md`

## TL;DR

`OplusSec` is an OPPO QSEE trusted app reached through ABL's QSEECom protocol. In this ABL it is used to read/write a 0x400-byte RPMB-backed `boot_info` blob.

Known command protocol:

```text
cmd 0    get OplusSec TA version / probe
cmd 9    read RPMB boot_info
cmd 10   write RPMB boot_info
```

Known boot_info format:

```text
OplusSec cmd 9 response:
  u32 out_len_or_object = 0x404  (as logged by qsee-buf/r32)
  u32 retval            = 0
  u8  boot_info[0x400]

boot_info+0x000..0x01f  early state fields (partly mapped below)
boot_info+0x020          model string used by VerifyUnlockToken new-struct path
boot_info+0x3e0..0x3ff  SHA256(boot_info[0x000..0x3df])
```

ABL validates the hash on read. On write, ABL reads existing `boot_info`, mutates early fields, recomputes the hash, and sends `cmd 10` with `u32 cmd=10 || boot_info[0x400]`.

## Static RE status

Applied Ghidra names:

```text
0x355c0 ReadRpmbBootInfo
0x35b30 OplusSecInvokeCommand
0x35840 Sha256RpmbBootInfoBody
0x3dd10 VerifyUnlockToken
0x3e440 FastbootUnlockVerify
```

`ReadRpmbBootInfo`:

- Sends command id `9` via `OplusSecInvokeCommand`.
- Expects out length `0x404`, retval `0`.
- Copies 0x400-byte payload to caller.
- Computes SHA-256 over payload `0x000..0x3df`.
- Compares against payload `0x3e0..0x3ff`.
- Logs `boot_info+0x08` as `last_bootmode`.

`VerifyUnlockToken`:

- Calls `ReadRpmbBootInfo` in new-struct token path when RPMB is enabled.
- Compares model string at `boot_info+0x20` against token `+0x12c`.

`FUN_00025340` AVB boot flow:

- Calls `ReadRpmbBootInfo`.
- Mutates early fields in the 0x400-byte payload.
- Calls `Sha256RpmbBootInfoBody`.
- Sends OplusSec command `10` to write updated boot_info.
- Evidence says it preserves `boot_info+0x20` model string; it does not rewrite model in the ordinary boot path.

## Log evidence from mode-0/mode-1 captures

Relevant log roots:

```text
/home/vivy/gbl-chainload/logs
/home/vivy/gbl-chainload-dirty/logs
```

Important examples:

```text
Mode 1-ish / fakelocked:
  /home/vivy/gbl-chainload/logs/20260513-022705_manual_manual_vd0be774/logfs/UefiLogSaved4.txt

Mode 0 / truly-unlocked-ish reference:
  /home/vivy/gbl-chainload-dirty/logs/20260508-204459_manual_fakelocked-debug-no-ebs-stock-images_vcc8a48e/logfs/UefiLogSaved4.txt

Another mode-0/transition sample:
  /home/vivy/gbl-chainload/logs/20260512-163636_manual_manual_vb26686e/logfs/UefiLogSaved2.txt
```

### OplusSec cmd 9 read shape from logs

Example `cmd 9` response line from `UefiLogSaved4.txt`:

```text
qsee-buf | h=4294901764 | dir=r | off=0 |
hex=04040000 00000000 00000000 01000000 01000000 01000000 00000000 02000000 00000000 00000000 4350483237343900...
```

Interpretation:

```text
response+0x00 u32 = 0x404
response+0x04 u32 = 0 retval
boot_info begins at response+0x08
```

For that sample:

```text
boot_info+0x00 = 0
boot_info+0x04 = 1
boot_info+0x08 = 1   last_bootmode (log agrees: last_bootmode:1)
boot_info+0x0c = 1
boot_info+0x10 = 0
boot_info+0x14 = 2
boot_info+0x18 = 0
boot_info+0x1c = 0
boot_info+0x20 = "CPH2749\0" model string
```

Across parsed logs, the model is consistently `CPH2749` for the NA Infiniti device. Mode-0 and mode-1 captures differ in the first 0x20 bytes, but not at the model field.

Observed first-eight dwords from cmd9 boot_info reads include:

```text
[0, 1, 1, 0, 0, 2, 0, 0]
[0, 1, 1, 1, 0, 2, 0, 0]
[0, 1, 0, 1, 1, 0, 0, 0]
[0, 1, 62, 1, 0, 0, 0, 0]
[0, 1, 62, 0, 0, 0, 0, 0]
```

Known correlation:

- `boot_info+0x08` is `last_bootmode`; logs print `0`, `1`, or `62` accordingly.
- `boot_info+0x20` is model, stable as `CPH2749` in all inspected NA captures.
- `set_boot_info_to_rpmb: secureboot_state:%d lock_state:%d boot_mode:%d tmp_boot_mode:%d` logs describe some early-field write inputs, but exact field mapping still needs final variable-offset cleanup in `FUN_00025340`.

### OplusSec cmd 10 write shape from logs

Example `cmd 10` send buffer:

```text
qsee-buf | h=4294901764 | dir=s | off=0 |
hex=04040000 0a000000 00000000 01000000 01000000 01000000 00000000 02000000 00000000 00000000 4350483237343900...
```

Interpretation:

```text
send+0x00 u32 = 0x404 command/input length marker
send+0x04 u32 = 10 command id
send+0x08 begins boot_info[0x400]
```

The cmd10 send payload preserves the model string at `boot_info+0x20`. Ordinary ABL boot writes therefore do not explain model mismatch unless a different path/TA/client mutates the model.

## What is still unknown

1. Exact names for `boot_info+0x00..0x1f`.
   - We know `+0x08 = last_bootmode`.
   - Candidate fields include secureboot state, lock state, boot_mode, tmp_boot_mode, project/region/abnormal-reboot state.
   - Need one more static pass through `FUN_00025340` around the `local_2470` boot_info struct writes.

2. Full `boot_info[0x40..0x3df]` content.
   - Current logs usually print only qsee-buf offsets 0, 64, and 128; enough for model but not full hash/body.
   - To recover full payload, update instrumentation to dump all `0x404` bytes for OplusSec cmd9/cmd10, not only first 192 bytes.

3. Whether Android userspace can call the same OplusSec TA.
   - If yes, a root-side read-only probe could call cmd9 directly.
   - If no, gbl-chainload/ABL instrumentation is the clean path.

## Answer to “what dump is required?”

Best dump for the OplusSec payload is **not** `oplusreserve1`; it is the OplusSec cmd9 output buffer.

Minimum useful capture per boot state:

```text
OplusSec cmd9 full response buffer, 0x404 bytes
OplusSec cmd10 full input buffer, 0x404 bytes, when present
```

States to capture:

```text
mode 0 truly-unlocked baseline
mode 1 fakelocked baseline
after DeepTest approval on CN
after first successful bootloader entry on CN
after fastboot flashing lock on CN
after Android boot post-lock on CN
```

Also capture, for correlation:

```text
oplusreserve1 full image
oplusreserve2 full image
misc full image
ocdt image
ro.build.version.ota
ro.product.name
board serial source
token block at oplusreserve1 4K LBA 1114 / offset 0x45a000
UnlockRecord at oplusreserve1 4K LBA 1187 / offset 0x4a3000
```

## Recommended next work

1. Instrument qsee-buf logging to emit the full 0x404 bytes for OplusSec cmd9/cmd10.
   - Current logs confirm structure but not full payload.
   - Read-only; do not modify buffers.

2. Finish static struct recovery in `FUN_00025340`.
   - Specifically track writes to the local `boot_info` buffer before cmd10.
   - Produce table: offset, write source, observed values from logs, likely meaning.

3. On CN failure investigation, prioritize token block diff first.
   - OplusSec model looks stable in NA mode 0/1 and ordinary ABL writes preserve it.
   - The most likely CN fastboot-loss mechanism remains token block invalidation/zeroing or one-shot reserve-state consumption, not boot_info model drift.

## Current interpretation

OplusSec `boot_info` is a QTEE/RPMB-backed OPPO boot metadata record. For fastboot unlock verification, only the model field at `+0x20` is known to matter. In the available NA captures, mode-0 and mode-1 change early state fields but keep model stable. Thus, for the CN post-relock fastboot-loss problem, OplusSec/model drift is now a secondary hypothesis unless CN dumps show `boot_info+0x20` diverging from token `+0x12c`.
