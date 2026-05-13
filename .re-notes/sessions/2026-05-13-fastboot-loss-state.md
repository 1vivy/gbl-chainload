# Post-relock fastboot-loss state isolation — 2026-05-13

Program: `LinuxLoader_infiniti.efi` in Ghidra project `gbl_root_canoe`.
Scope: Handover RE-1/RE-2/RE-3 first pass. No device writes performed.

## Applied Ghidra annotations

- `0x3e440` → `FastbootUnlockVerify`
- `0x3dd10` → `VerifyUnlockToken`
- `0x355c0` → `ReadRpmbBootInfo`
- `0x35b30` → `OplusSecInvokeCommand`
- `0x35840` → `Sha256RpmbBootInfoBody`
- Decompiler comment at `LinuxLoaderEntry:0x6d78` documenting the fastboot entry gate and reboot deny path.

## RE-1: token binding payload

CN freshly-unlocked token from `images/oplusreserve1_cn_freshlyunlocked.img` at 4K LBA `1114` / offset `0x45a000`:

```text
token+0x100..0x107 serial       = "be55ac8a"
token+0x108..0x10b marker       = "0002"
token+0x10c        permission   = "1"
token+0x10d..0x12b binding31    = "0000000000000000000000000000000"
token+0x12c..0x13b model        = "PLK110##########"
```

The 32-byte field signed by new-struct verification is:

```text
token+0x10c..0x12b = "10000000000000000000000000000000"
```

Important result: in this sample the supposed 31-byte binding payload is all ASCII zeroes. `VerifyUnlockToken` does **not** compare these bytes against OTA version, IMEI, account UID, or any local property. It only:

1. checks `token+0x10c == '1'`,
2. copies `token+0x10c..0x12b` to `DAT_000bae78`, and
3. includes the 32-byte field in the RSA-covered message.

New-struct signed message is exactly 60 bytes:

```text
serial[8] || marker[4] || permission_and_binding[32] || model[16]

"be55ac8a000210000000000000000000000000000000PLK110##########"
```

SHA-256 of the 60-byte signed message for reference:

```text
e8ec9e76f2fd5aa927a713d4640a71e094e1529d1eba778a20c76a44e2cfdf93
```

Implication for H3: there is no evidence in ABL that OTA-version drift is checked locally. If OTA/version binding exists, it is purely server-side in the signed token issuance policy, not a post-issuance ABL comparison. A valid existing token should not become invalid because Android properties changed, unless the token block itself changes or the RPMB model/serial/key selection changes.

## RE-2: fastboot gate caller and deny path

Only xref to `FastbootUnlockVerify`:

```text
LinuxLoaderEntry:0x6d78  bl FastbootUnlockVerify
```

Assembly context:

```text
00006d78: bl 0x0003e440              ; FastbootUnlockVerify()
00006d7c: cbz x0,0x00006d98          ; return 0 -> continue fastboot init
00006d80: adrp x1,0x62000
00006d84: add x1,x1,#0x6fb           ; "fastboot_unlock_verify error and reboot."
00006d88: mov w0,#0x80000000
00006d8c: bl 0x00001acc              ; log error
00006d90: mov w0,#0x3e
00006d94: bl 0x00029268              ; reboot / boot-mode transition helper
00006d98: ...                        ; "Fastboot Build Info", fastboot init follows
```

This exactly matches the observed symptom: nonzero verifier result reboots before USB fastboot enumeration or UI.

State inputs consumed by this gate:

- `ReadSecurityState` secureboot bit.
- `ud version` blobs.
- `engineering_cdt` beta RSA path.
- OCDT verify (`bid`, `prj`) and whitelist.
- `oplusreserve1` token block plus board serial and RPMB model via `VerifyUnlockToken`.

## RE-3: RPMB boot_info / model reader

`ReadRpmbBootInfo` (`0x355c0`) behavior:

1. Builds command id `9`.
2. Calls `OplusSecInvokeCommand(cmd=9, in_len=4, out_len<=0x1000)`.
3. Expects output length `0x404` and retval dword `0`.
4. Copies the following `0x400` bytes to caller.
5. Computes SHA-256 over `boot_info[0x000..0x3df]`.
6. Compares digest to `boot_info[0x3e0..0x3ff]`.

`VerifyUnlockToken` passes a 0x400 local buffer to `ReadRpmbBootInfo`, then copies 16 bytes from `boot_info+0x20` and compares that NUL-terminated model string against `token+0x12c`.

Confirmed useful boot_info offsets:

```text
0x008  last_bootmode (logged by ReadRpmbBootInfo)
0x020  model string used by VerifyUnlockToken new-struct path
0x3e0  SHA-256 over boot_info[0x000..0x3df]
```

`FUN_00025340` / AVB boot flow also reads and writes this same boot_info via:

```text
ReadRpmbBootInfo(...)
Sha256RpmbBootInfoBody(...)
OplusSecInvokeCommand(cmd=10, in_len=0x404, ...)
```

The write path modifies early boot_info state fields in the first `0x20` bytes (secureboot/lock/boot-mode/project-ish fields), recomputes the hash, and writes command id `10`. It does **not** appear to modify `boot_info+0x20` model; it preserves the existing model string read from RPMB.

Implication for H2: plain ABL boot_info updates/lock-state writes are not expected to change the model field, because they preserve the 0x400-byte blob and only update fields before `+0x20`. Model mismatch remains plausible for cross-region/provisioning drift, but not as an automatic consequence of the observed ABL command-10 write path.

## H4 quick static check

Searches for literal UnlockRecord magic bytes (`8a 97 9c 93` and `93 9c 97 8a`) found no standalone data matches. Known readers remain the `LinuxLoaderEntry` initializer and `UpdateUnlockRecord` flow via `BuildUnlockRecordHashPayload`.

This does not fully prove no computed/immediate compare exists, but it further weakens H4.

## Updated state-transition rows

| State surface | Field/region | What changes on `flashing lock`? | Consumed by which ABL function on next boot? | Where does it gate? |
|---|---|---|---|---|
| RPMB DevInfo | `is_unlocked` byte | expected flipped to 0 by lock | `ReadSecurityState` / VB protocol users; not a token-pass substitute | affects security/lock reporting; Gate 4 still requires token on CN |
| RPMB boot_info | model at `+0x20` | ABL cmd-10 path preserves it; lock overlap not seen in this path | `ReadRpmbBootInfo` → `VerifyUnlockToken` | new-struct model comparison only |
| Token block | RSA signature | TBD by CN pre/post-lock diff | `VerifyUnlockToken` | always, after Gate 3 fall-through |
| Token block | model at `+0x12c` | TBD by CN pre/post-lock diff | compared against `boot_info+0x20` | new-struct only |
| Token block | `+0x10d..+0x12b` binding31 | CN sample is all ASCII zeroes; no local comparison found | included in RSA message; copied to `DAT_000bae78` | only via RSA validity / permission byte at +0x10c |
| Token block | serial at `+0x100` | TBD by CN pre/post-lock diff | compared against `FUN_0001996c` board serial | always |
| UnlockRecord | full | TBD by CN diff | known init/update only; no fastboot gate found | likely none |
| OCDT | `bid`, `prj` | expected unchanged | `verify_ocdt` → `FastbootUnlockVerify` | Gate 3 whitelist and token key selection |

## Current best narrowing after RE-1/2/3

Most likely causes for post-relock CN fastboot loss are now reduced to:

1. **Token block is zeroed/invalidated by lock or one-shot consumption** (H1/H5). This is the leading candidate because CN success depends on Gate 4 and NA control has a zero token only because Gate 3 prj wins.
2. **RPMB boot_info model mismatch** due to cross-region/provisioning drift, not ordinary ABL boot_info command-10 updates.
3. **Board serial source drift** remains open until `FUN_0001996c` is reversed.

H3 (local OTA-version comparison) is currently weak: the sample binding field is ASCII zeroes and ABL does not inspect it beyond RSA coverage and global copy.
