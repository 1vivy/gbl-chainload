# gbl-chainload

EFI System Partition (EFISP) chainloader for OnePlus/Oppo devices using Qualcomm's GBL/EFISP load mechanism. Patches the active-slot ABL in memory, installs targeted protocol hooks, and hands off to the patched ABL.

## Status

v2 architecture in flight. See `docs/superpowers/specs/` for the design and `docs/superpowers/plans/` for the implementation plan series.

Working artifacts: `dist/mode-0.efi` (pass-through observation build) and `dist/mode-1.efi` (protocol-hook fakelock via `QCOM_VERIFIEDBOOT_PROTOCOL` mutation; KM/Oplus see locked/green when stock images verify cleanly).

## Modes

- **mode-0** — pass-through observation build. Patch engine + logfs only; no protocol hooks, no patch9. Useful for capturing logs against unmodified ABL verification behavior.
- **mode-1** — protocol-hook fakelock. ABL sees locked DeviceInfo and builds KM SET_ROT/SET_BOOT_STATE off that view.
- **mode-2** *(not yet implemented)* — TA-payload spoof at QSEE/SPSS boundaries; ABL stays honest; per-OTA typed-struct profile.
- **mode-3** *(not yet implemented)* — universal baseline only; minimal experiment to gauge KM root-cert leaf survival.

## Build

```bash
./scripts/build.sh --mode 0               # observation build (no hooks, no patch9)
./scripts/build.sh --mode 1               # fakelock production silent
./scripts/build.sh --mode 1 --auto --debug --verbose   # fakelock dev capture
```

## Logging

Three primitives from `GblChainloadPkg/Include/Library/GblLog.h` (ASCII format strings, drop the `L` prefix):

| Macro / call | Build flag | Goes to screen? | Goes to UefiLog<N>.txt? |
|--------------|------------|------------------|--------------------------|
| `Print(L"...")` | always | yes | yes |
| `GBL_INFO("...")` | `GBL_DEBUG=0` (prod) | no | yes (via `ReportStatusCode` → UART) |
| `GBL_INFO("...")` | `GBL_DEBUG=1` (`--debug`) | yes (via AsciiPrint) | yes |
| `VERBOSE("...")` | `GBL_VERBOSE=0` | — (compile-stripped to no-op) | — |
| `VERBOSE("...")` | `GBL_VERBOSE=1` (`--verbose`) | yes (via AsciiPrint) | yes |

`DEBUG((DEBUG_ERROR, "..."))` and `DEBUG((DEBUG_WARN, "..."))` are kept for unconditional error / warning paths (route through the same `ReportStatusCode` path).

### When to use which

- **`Print(L"...")`** — content the user MUST see regardless of build: user prompts ("Hold VolUp within 3s..."), fatal error messages ("LOGFS PARTITION NOT FOUND"). Screen-visible, also lands in UefiLog. Use sparingly.

- **`GBL_INFO("...")`** — semantic events relevant to verifying correctness: mutations our hooks perform (`vb-fakelock | is_unlocked 1->0`), swallows (`vb-rwstate | swallowed`), crypto outputs on intercepted commands (`qsee-km | SET_ROT | rotDigest=...`, `spss-bootstate | unlocked=0 | color=0`), install banners, BootFlow status. Silent on prod screen, visible under `--debug`, always in UefiLog. **This is the "did the hook do what I expect?" channel.**

- **`VERBOSE("...")`** — raw pass-through traces: every qsee call, every scm syscall, per-call hex dumps, scan-loop iterations. Compile-stripped from prod and `--debug` builds; only present in `--verbose`. **Don't use it for anything you'd want to see in prod.**

### Classification rules

When adding a new emit in a hook or patch:

- Is it a **mutation** the hook performs, or a **swallow** the hook executes, or a **crypto output on an intercepted command**? → `GBL_INFO`.
- Is it a pass-through query (READ / GET / probe) or a per-call raw dump? → `VERBOSE`.
- Is it protocol-internal marshalling (Mink op decoding, vtable dispatch tracing) with no semantic payload? → **don't emit at all**. Both prod and `--verbose` lose nothing.
- Is it a user prompt or an error that MUST be visible? → `Print(L"...")`.

### Format string gotchas

- All `GBL_INFO` / `VERBOSE` format strings are CHAR8 ASCII (`"foo=%u\n"`, no `L` prefix).
- `%s` consumes CHAR8\* in `GBL_INFO` / `VERBOSE` / `AsciiPrint`. To print a CHAR16\* string, convert it to ASCII first via `UnicodeStrToAsciiStrS` into a local buffer.
- `%a` always means ASCII string regardless of macro choice.

### What's NOT in the framework anymore

The PR #19 minimal design dropped these from PR #17:

- `DebugSink` (the `gST->ConOut->OutputString` hook that mirrored to a logfs file)
- `GblChainload_Boot<N>.txt` (per-boot logfs mirror file)
- `GBL_DBG_LOGFS_ONLY` error-level bit
- `GblDebugLib` shim library
- The screen-mask infrastructure (`gGblScreenMask`, `LogFsSetScreenMask`)

`UefiLog<N>.txt` (rotated by stock BDS via `LogFsLib`'s rotation) is the sole persistent log destination. Pull it from the `logfs` partition or read `/proc/bootloader_log` from a booted recovery / Android.

## Repo conventions

- `GblChainloadPkg/Library/DynamicPatchLib/{universal,oem,mode_1}/` — patches scoped by applicability.
- `GblChainloadPkg/Library/ProtocolHookLib/UniversalBaseline.c` — hooks every mode ships.
- `GblChainloadPkg/Library/ProtocolHookLib/Mode1Overlay.c` — mode-1-specific hooks atop baseline.
