# 2026-05-08 — KM cmd-id revalidation + TA enumeration on canoe

## Anchor

User flagged `gbl_root_canoe`'s cmd-id table as derived from upstream
edk2 with potential drift. Cross-verification done against
`LinuxLoader_infiniti.efi` (canoe ABL, 725 funcs, auto-analyzed).

## Verified

### KeyMasterStartApp (FUN_00035460)

Decompile shows:
1. `LocateProtocol(gQcomQseecomProtocolGuid)` → handle.
2. `protocol.slot_0x8(handle, "keymaster", &AppId)` → QseecomStartApp.
3. `protocol.slot_0x18(handle, AppId, {cmdId=0x200}, 4, &rspBuf, 0x14)` →
   QseecomSendCmd with hardcoded `0x200` literal.
4. Response check: `status==0 && rspBuf[4]_u32 >= 2` (major ver gate).
5. Log: `"KeyMasterStartApp success AppId: 0x%x, Major: %d"`.

Confirms cmd `0x200` semantic (probe / get-version) byte-identical to
our `qsee-km` decoder + `keymaster_wire.h`'s implicit ordering. The
cmd-id is a literal at the dispatch site, NOT a #define — so no name
appears in the binary, just the constant.

Runtime correlation: device captures show `ver=3.0.3 buildId=0x7C` for
canoe — passes the `Major >= 2` gate trivially.

### TA inventory in canoe binary

Strings + xrefs:
- `keymaster`              — loaded by FUN_00035460 (KeyMasterStartApp).
- `secretkeeper`           @ 0x5cc1d
- `secretkeeper_a`         @ 0x6cf9c
- `secretkeeper_b`         @ 0x5991d
- `audioreach`             @ 0x5cec7 — referenced from FUN_00004e50.
- `secretkeeper_public_key`@ 0x63640 — Secretkeeper RPC key constant.

`SecretkeeperLoadApp` (function not yet renamed) tries `_a` first, falls
back to `_b` (logs `"SecretkeeperLoadApp: Could not load form _a"` and
`"_b"`). Matches our captured handle 0xFFFF0004 = `secretkeeper_a`.

### SPSS keymint share

Function `FUN_00025340` references three strings:
- `"No SPSS EFI protocol, not sharing keymint info\n"` (0x649a9)
- `"ShareKeyMintInfoWithSPU failed: %r\n"` (0x6742c) — caller log
- `"SPSSDxe_ShareKeyMintInfo failed: %r\n"` (0x6ef9c) — TA-side log

This is a `gQcomSPSSDxeProtocolGuid` consumer that mirrors the
keymaster RoT to the SPSS (Secure Processor Subsystem) so Secretkeeper
on SPSS gets the same RoT view as the AP-side TZ. Worth tracing for
mode-1 — mutating only the AP-side RoT and leaving SPSS stale could
produce internal inconsistency that breaks DICE chain.

## Open follow-ups

1. **Appid `0xFFFF0002`** still unidentified. The `qsee-start` hook
   should capture it on a normal-Linux boot path (recovery loads only
   keymaster+secretkeeper; full Linux boot loads audioreach + others).
2. **`FUN_00004e50` audioreach loader** — decompile to confirm it's the
   audioreach `QseecomStartApp` site and capture the AppId assignment.
3. **`FUN_00025340` SPSS share** — decompile to learn what RoT bytes
   get mirrored. If it sends a copy of the same SET_BOOT_STATE struct,
   mode-1 must hook both AP-side KM and SPSS share.
4. **Color enum on canoe vs upstream** — captured `0x208 SET_BOOT_STATE
   color=1` for unlocked, but upstream enum says `2 (ORANGE)`. Locate
   the color-set source on the binary side to identify the mapping
   canoe actually uses.

## Methodology note

`gbl_root_canoe`'s cmd-id table was derived by reading
`KeymasterClient.h` from `external/edk2-uefi.lnx.5.0.r10-rel` AND
confirming against device wire dumps. The "differed a tiny bit"
observation is real: upstream KMv3 → canoe KMv3.0.3 may have additional
or renamed cmds. Our `qsee-km` decoder has 9 known cmds (0x200/0x201/
0x202/0x203/0x204/0x207/0x208/0x211/0x218/0x219) — anything outside
that set falls through to the generic `qsee | ...` line and shows up
as `cmd=0x???` with no decoded interpretation.
