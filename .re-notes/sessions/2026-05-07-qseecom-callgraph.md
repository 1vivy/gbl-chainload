# 2026-05-07 — QSEE call-graph mapping (mihon LinuxLoader.efi)

## State

- Ghidra project: `gbl_root_canoe` (PID 17149, UDS).
- Programs in project:
  - `LinuxLoader.efi` (mihon, version 11) — **875 functions analyzed**, used as the reference.
  - `LinuxLoader_infiniti.efi` (canoe, version 3) — 0 functions, NOT yet auto-analyzed.
  - `LinuxLoader_superturtles_patched.efi` (version 4) — not opened.
  - `OplusSecurityDxe.efi` (version 1) — 113 functions, opened but not yet investigated.
- No prior `.re-notes/` or `docs/re/` content existed.

## What I confirmed

- Hook target = `QCOM_QSEECOM_PROTOCOL.QseecomSendCmd` = vtable slot **0x18**.
  Confirmed by FUN_0004aed4 (Send_Idmanager_Cmd) which calls
  `(**(code **)(local_70 + 0x18))(local_70, AppId, Send=&DAT_000d8cc0, 0x848,
  Rsp=&DAT_000d9508, 0x808)` after `LocateProtocol(&gQcomQseecomProtocolGuid)`.
- `QseecomStartApp` = vtable slot **0x8**. Confirmed by FUN_0004ad60 (LoadSecidsTA)
  which calls `protocol->slot_0x8("idmanager", &DAT_0010620c)`.
- Protocol GUID address = `&DAT_000cb314`.
- gBS = `DAT_00106400`; LocateProtocol = vtable+0x140 ✓ matches EFI_BOOT_SERVICES.
- Handle values in our log are AppIds, issued sequentially by load order. Not fixed
  Qualcomm IDs. Map of TA name → AppId is the missing link, achievable by hooking
  StartApp too (planned but not yet done).

## TA name → caller name → caller address

(All in `LinuxLoader.efi`. Names lifted from string literals and applied function
labels.)

| TA name string @addr   | StartApp/SendCmd caller     | Notes |
|------------------------|-----------------------------|-------|
| `idmanager` @0x64750   | FUN_0004ad60 (LoadSecidsTA) | StartApp(idmanager); FUN_0004aed4=Send_Idmanager_Cmd does the SendCmd at vtbl+0x18. |
| `keymaster` @0x6c459   | FUN_00031ce8 (KeyMasterStartApp) | StartApp(keymaster); SendCmd downstream not yet enumerated. |
| `secretkeeper` @0x69f1b | FUN_0001153c               | Massive function; loads `secretkeeper` and `secretkeeper_b`. |
| `secretkeeper_b` @0x66911 | FUN_0001153c              | B-slot variant. |

## Pending

- [ ] Run auto-analysis on `LinuxLoader_infiniti.efi` and re-confirm names port over
      (likely yes; OEM bring-up is shared).
- [ ] Apply labels in mihon: rename FUN_0004aed4 → `Send_Idmanager_Cmd`,
      FUN_0004ad60 → `LoadSecidsTA`, FUN_00031ce8 → `KeyMasterStartApp`. (Not yet
      applied — keeping reads of mihon untouched until user OK's writing into the
      project.)
- [ ] Locate and label the singleton `QseecomSendCmd` body — it's the function
      installed at vtbl+0x18 of `gQcomQseecomProtocolGuid` instance. Likely lives
      near KeyMaster/QSEE init code. Find via xrefs to a global function pointer
      that's stored at the +0x18 offset of the protocol struct.
- [ ] Decompile FUN_0001153c (270 KB) — chunk-read from
      `/home/vivy/.claude/projects/-home-vivy-gbl-chainload/aebe5b89-6704-4b5b-b695-423213b3ff83/tool-results/mcp-ghidra-mcp-decompile_function-1778173124343.txt`
      or scope it down via `disassemble_function` to specific blocks.
- [ ] Look at OplusSecurityDxe.efi (113 funcs) for the Phoenix oplusreserve1 cmd
      handlers — the handle-4 / cmd-0x004 / cmd-0x404 page traffic almost certainly
      maps into here.
- [ ] Multiple-boot diff to classify the two SHA-256s (det vs nonce).
