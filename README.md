# gbl-chainload

EFI System Partition (EFISP) chainloader for OnePlus/Oppo devices using Qualcomm's GBL/EFISP load mechanism. Patches the active-slot ABL in memory, installs targeted protocol hooks, and hands off to the patched ABL.

## Status

v2 shim is usable for mode-0 and mode-1. Project documentation, reverse-engineering findings, and the current milestone marker live in [`docs/project/`](docs/project/).

Working artifacts: `dist/mode-0.efi` (unlocked observation + universal preservation build) and `dist/mode-1.efi` (protocol-hook fakelock via `QCOM_VERIFIEDBOOT_PROTOCOL` mutation; KM/Oplus see locked/green when stock images verify cleanly).

Mode-1 supports the "stock recovery + custom system" use case by default. Custom recovery + normal boot requires a disk-side graft of stock vbmeta — see [`docs/project/next-milestone.md`](docs/project/next-milestone.md). Both a host script and a device-side companion module remain next-milestone work; neither ships today.

## Modes

- **mode-0** — unlocked observation + universal preservation build. Installs protocol hooks for logging and for the narrow preservation baseline: drop TZ soft-fuse advancement and swallow `oplusreserve1` / `opporeserve1` writes. VB lock-state and OplusSec writes pass through so stock ABL can run the real relock procedure.
- **mode-1** — protocol-hook fakelock. ABL sees locked DeviceInfo and builds KM SET_ROT/SET_BOOT_STATE off that view.
- **mode-2** *(not yet implemented)* — TA-payload spoof at QSEE/SPSS boundaries; ABL stays honest; per-OTA typed-struct profile.

## Build

```bash
./scripts/build.sh --mode 0               # unlocked observation + preservation baseline
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

## Repo conventions

- `GblChainloadPkg/Library/DynamicPatchLib/{universal,oem,mode_1}/` — patches scoped by applicability.
- `GblChainloadPkg/Library/ProtocolHookLib/UniversalBaseline.c` — narrow policies every mode ships.
- `GblChainloadPkg/Library/ProtocolHookLib/Mode1Overlay.c` — mode-1-specific hooks atop baseline.

## FastbootLib surface

The bundled edk2 FastbootLib is trimmed for RAM-loaded gbl-chainload testing and scripts.

Useful getvars:

- `gbl-chainload_mode` — `mode-0`, `mode-1`, etc.
- `gbl-chainload_date`
- `gbl-chainload_auto`
- `gbl-chainload_debug`
- `gbl-chainload_verbose`
- `unlocked`
- `oem-unlock-allowed`
- `vbmeta:capabilities`
- `vbmeta:slot`
- `vbmeta:warning`
- `vbmeta:<partition>:status`
- `vbmeta:<partition>:descriptor-type`

Useful commands:

- `fastboot stage <file>` — stage an image for `boot`, `oem boot-efi`, or other staged-data commands.
- `fastboot oem escape` — leave gbl-chainload FastbootLib and continue into patched ABL.
- `fastboot oem boot-efi` — boot the currently staged EFI image with `LoadImage()` / `StartImage()`.
- `fastboot oem oem-unlock-toggle` — enable the FRP OEM-unlock-allowed bit; second use is a no-op.
- `fastboot flashing lock` / `fastboot flashing unlock` — update DevInfo lock state while skipping forced recovery wipe.

Fastboot screen additions:

- `DATE - ...` build timestamp line.
- `OEM UNLOCK ALLOWED - yes/no` state line.
- `AVB WARNING - ...` warning line when lightweight vbmeta probing detects a risky state.
- `Enable OEM unlock` menu action.
- `Escape` menu action.

There is also an untested `Boot ESP` menu option intended for directly booting operating systems from USB.

## Mode-0 reserve preservation test plan

Use only RAM-load testing for gbl-chainload itself:

1. Boot/install the mode-0 chainload path.
2. Install stock firmware.
3. Perform a relock through the stock bootloader flow.
4. Unlock again and pull logfs.
5. Confirm a reserve write swallow was logged for the token block, e.g.
   `blockio | op=write-swallow | reason=token-zero-write | p=oplusreserve1`.

The token-zero intercept is also printed as vital user-visible output in every
build:

`GBL: intercepted reserve token zeroing on oplusreserve1 LBA 1114; token preserved`

That line appears when the relock path attempts to zero the token block.
