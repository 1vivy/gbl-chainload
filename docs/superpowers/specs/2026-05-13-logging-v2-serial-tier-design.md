# Logging v2 — Serial-Tier Verbose, Co-opted UART Buffer / UefiLog

**Status:** Draft (2026-05-13)
**Supersedes:** the Phase-1c log-stream-split design (`GBL_DBG_LOGFS_ONLY`, screen-mask gymnastics) shipped as commits `0694f53..e5538b8` on `feature/cleanup-p1c-log-stream-split`.

---

## 1. Goals

1. **Prod build (no flags):** silent screen. Errors and user prompts only. Higher-level debug content captured somewhere durable (UART buffer → UefiLog) but never on screen.
2. **`--debug` build:** higher-level debug content (intercept performing, swallow performing, patch outcomes, hook install, per-hook *summary*) visible on screen and persisted.
3. **`--verbose` build (compile-time, additive on top of `--debug`):** per-hook *payload hex* captured to UefiLog through the QCOM SerialPortLib path. Never on screen — screen and UefiLog have independent backends, this design uses that fact instead of fighting it.
4. **Correlation** between debug-tier summary lines and verbose-tier payload hex must be trivial host-side (grep on a shared ID).
5. **Compile-time stripping** of the verbose-tier macro body in non-verbose builds. Zero code, zero perf in prod and `--debug` artifacts.

---

## 2. Mechanism we're co-opting

QCOM XBLCore allocates a **RAM ring buffer** (`UefiInfoBlk->UartLogBufferPtr`, default 32 KiB, configurable to 1 MiB via `uefiplat.cfg`'s `UARTLogBufferSize`). `SerialPortLib` writes append to this buffer for every UEFI image in the chain (XBL → gbl-chainload → patched ABL → recovery's stub).

Stock BDS in `PlatformBdsLib::WriteLogBufToPartition` dumps the buffer to `\UefiLog<N>.txt` on the `logfs` partition at a pre-boot milestone, where `N = BootCycleCount % 5`. The patched ABL's BDS does this for the chain we care about.

**Key fact:** the UART buffer is fed by `SerialPortWrite` — **not** by `gST->ConOut->OutputString`. Screen output goes through ConOut; UefiLog content goes through the UART path. They share input only through DebugLib variants that hit both — not by default. The patched ABL's stock DebugLib is serial-only, which is why UefiLog accumulates ABL content while the framebuffer stays quiet post-handoff.

This design uses that split directly:
- Things we want on screen → ConOut (current path through `GblDebugLib`).
- Things we want in UefiLog only → SerialPortLib (new path: `SERIAL_DBG` macro).

The shared header `QcomPkg/Include/Library/SerialPortShLibInstall.h` exports `SerialBufferReInit(buf, size)` and `SerialPortDrainToDest(...)` — the resize call is reachable from a stage-2 EFI app linking `SerialPortLib`.

---

## 3. Tier definitions

| Tier | How to emit | Routes to | When |
|------|-------------|-----------|------|
| **ERROR** | `Print(L"...")` or `DEBUG((DEBUG_ERROR, "..."))` | ConOut → screen + UART → UefiLog | Always. Failures, user prompts (`Hold VolUp...`), critical messages. |
| **DEBUG** | `DEBUG((DEBUG_INFO, "..."))` | ConOut → screen *under `--debug`* + UART → UefiLog | Intercept performing, swallow performing, patch outcomes, hook install. Per-hook one-line *summary* with `id=N`. |
| **VERBOSE** | `SERIAL_DBG(fmt, ...)` (new macro) | SerialPortLib → UART → UefiLog only | Per-hook payload hex with same `id=N`. Compile-time gated by `GBL_VERBOSE=1` — body absent in non-verbose builds. |

For ERROR and DEBUG to reach BOTH screen and UefiLog through a single `DEBUG()` call site, `GblDebugLib::DebugPrint` calls `SerialPortWrite` in addition to its existing `gST->ConOut->OutputString` path. One emit, both destinations. Without this tee, summary lines that fire post-handoff (when ConOut → screen is quiet on canoe because the patched ABL's stock DebugLib is serial-only) would never reach UefiLog and the `[id=N]` correlation with verbose payloads would be broken.

`gGblScreenMask` controls ConOut-path filtering and stays as-is:
- Default (prod): `DEBUG_ERROR`.
- `--debug`: `DEBUG_ERROR | DEBUG_WARN | DEBUG_INFO`.
- `--verbose` adds nothing — the verbose tier doesn't use this path.

---

## 4. Components and changes

### 4.1 New: `SERIAL_DBG` macro and helper

Lives in `GblChainloadPkg/Include/Library/GblSerialDbg.h`:

```c
#ifndef GBL_VERBOSE
# define GBL_VERBOSE 0
#endif

#if (GBL_VERBOSE == 1)
  VOID EFIAPI GblSerialDbgPrint (IN CONST CHAR8 *Fmt, ...);
# define SERIAL_DBG(fmt, ...)  GblSerialDbgPrint (fmt, ##__VA_ARGS__)
#else
# define SERIAL_DBG(fmt, ...)  do { (void)0; } while (0)
#endif
```

`GblSerialDbgPrint` formats with `AsciiVSPrint`, then calls `SerialPortWrite (Buf, Len)`. No ConOut, no LogFsWrite, no screen-mask check. Direct path to UART buffer.

Implementation in `GblChainloadPkg/Library/GblSerialDbgLib/GblSerialDbgLib.c`. Library class `GblSerialDbgLib` mapped in DSC.

### 4.2 New: UART buffer resize on entry

Lives in `GblChainloadPkg/Library/GblSerialDbgLib/UartBufferResize.c`:

```c
EFI_STATUS GblResizeUartLogBuffer (UINTN NewBytes);
```

Called once from `CommonEarlyInit` BEFORE the first `DEBUG()` of this image. Behavior:

1. `AllocatePagesRuntimeServiceData(EFI_SIZE_TO_PAGES(NewBytes))`.
2. Locate `UefiInfoBlk` (via the same GetInfoBlkPtr the BSP uses — public symbol in `QcomPkg/Include/UefiInfoBlk.h`).
3. Read current `UartLogBufferPtr/Len`, copy existing content forward into new buffer (so XBL's content isn't lost), zero the rest.
4. Write `UartLogBufferPtr = NewBuf`, `UartLogBufferLen = NewBytes` in `UefiInfoBlk`.
5. Call `SerialBufferReInit(NewBuf, NewBytes)`.
6. Return EFI_SUCCESS or the failure status.

Allocation failure: surface via `Print()` so the user sees the warning on screen, keep stock 32 KiB. Verbose under `--verbose` then wraps when the ring fills. This is a degradation mode, not a designed-for path — verify on-device that 1 MiB consistently succeeds on canoe before declaring the resize step robust.

**Target size:** 1 MiB (`0x100000`). Allocated as `RuntimeServiceData` so it survives EBS — important because the patched ABL's BDS won't have flushed yet.

### 4.3 Drop `GBL_DBG_LOGFS_ONLY` infrastructure

- Remove the `0x10000000` bit definition from `GblChainloadPkg/Include/Library/LogFsLib.h`.
- `PcdDebugPrintErrorLevel` revert to `0x80000042` unconditionally. Remove the `--verbose` widening in `GblChainloadPkg.dsc`.
- Re-tag `ProtocolHookLib` per-call traces:
  - **Summary line** (`qsee | id=%u | cmd=0x%x | sl=%u | rl=%u | st=%r`): `DEBUG_INFO`.
  - **Payload hex** (`qsee-buf | id=%u | dir=s | off=%u | hex=%a`): `SERIAL_DBG`.
- Same pattern for `ScmHook`, `VerifiedBootHook`, `QseecomHook`, `SpssHook`, `Mode1Overlay`.
- Tests/052 checks 7–9 (the `GBL_DBG_LOGFS_ONLY` assertions): delete them. Add a new check that `SERIAL_DBG` is used for payload hex in ProtocolHookLib instead of `DEBUG_VERBOSE`.

### 4.4 Per-hook ID counters

Per-hook category, declared `STATIC UINT32 g<Hook>CallId = 0;` in each hook file. Incremented on hook entry; same value used in both the `DEBUG_INFO` summary and any `SERIAL_DBG` payload lines for that call. Reset implicit (new boot zeroes it). Categories: `Qsee`, `Scm`, `Vb`, `Spss`. IDs collide across categories — the trace prefix (`qsee | `, `scm | `, etc.) disambiguates.

### 4.5 `--verbose` build flag

`scripts/build.sh`:
- Keep `--verbose` CLI flag, sets `VERBOSE=1`.
- Pass to docker as `GBL_VERBOSE` env var (already wired).
- DSC reads `GBL_VERBOSE`, sets it as a build-option `#define` for `GblSerialDbgLib`.

Dist artifact naming unchanged: `mode-1-auto-debug-verbose.efi` continues to work.

### 4.6 Things that stay

- `LogFsClose()` before `LoadImage` — partition handle release rule. Mandatory.
- `LogFsInstallDebugSink` retained across `LoadImage` — mask filter on patched-ABL ConOut emits.
- `LogFsLib` EBS callback — harmless defense-in-depth, no-ops when logfs closed.
- `gbl-chainload_Boot<N>.txt` rotation, sink mirror, all pre-handoff plumbing.
- `--debug` flag — widens `gGblScreenMask` to admit `INFO/WARN`, enables `LOGFS_PROBE` Prints.

---

## 5. Data flow

### 5.1 Pre-handoff (gbl-chainload running)

```
GblDebugLib::DebugPrint
   ├── sets gDbgCurrentLevel
   └── gST->ConOut->OutputString  (= HookedOutputString)
        ├── mask check → gOriginalOutputString → screen
        └── LogFsWrite → gbl-chainload_Boot<N>.txt

SERIAL_DBG (only in --verbose builds)
   └── AsciiVSPrint → SerialPortWrite → UART ring buffer
```

`SERIAL_DBG` calls pre-handoff are rare (no hooks have fired yet) but the path is the same.

### 5.2 Post-handoff (patched ABL running)

```
Patched ABL DEBUG (stock QCOM DebugLib, serial-only)
   └── SerialPortWrite → UART ring buffer → UefiLog

Our hook code (statically linked into patched ABL via patches)
   ├── DEBUG_INFO summary line  → GblDebugLib::DebugPrint
   │       ├── HookedOutputString → mask check → screen (silent post-handoff
   │       │                                       on canoe; sink mirror to
   │       │                                       logfs is no-op because
   │       │                                       LogFsClose ran pre-handoff)
   │       └── SerialPortWrite (tee, §3) → UART ring buffer → UefiLog
   └── SERIAL_DBG payload line  → SerialPortWrite → UART ring buffer → UefiLog
```

Summary line and payload lines for the same hook call share `id=N` and both land in UefiLog — host-side correlation works by grep-and-pair.

### 5.3 BDS flush

Stock patched-ABL BDS calls `PlatformBdsLib::WriteLogBufToPartition` at pre-boot. Reads current `UefiInfoBlk->UartLogBufferPtr` (our 1 MiB) and dumps full contents to `\UefiLog<N>.txt`. Subsequent OS boot doesn't touch this file until next boot's BDS runs.

---

## 6. Testing

### 6.1 Host-side (CI, no device)

- `tests/052_log_stream_split.sh` rewrite:
  - Drop checks 7, 8, 9 (GBL_DBG_LOGFS_ONLY-specific).
  - Add: `SERIAL_DBG` defined in `Include/Library/GblSerialDbg.h`, compile-gated on `GBL_VERBOSE`.
  - Add: `ProtocolHookLib` per-hook traces have payload lines via `SERIAL_DBG`, summary lines via `DEBUG_INFO` with `id=` prefix.
  - Add: `GblResizeUartLogBuffer` called from `CommonEarlyInit` before first DEBUG.
  - Keep: sink retained across `LoadImage`, no `LogFsClose` removed from BootFlow pre-handoff path (the close stays), EBS callback registered.
- `tests/010_build_smoke.sh` — already builds 4 variants; add a check that `mode-1-auto-debug-verbose.efi` contains the `GblSerialDbgPrint` symbol and the other variants do NOT (strip verification).

### 6.2 On-device (manual, per the project's stage+boot-efi test loop)

- Stage `mode-1-auto-debug-verbose.efi`, boot-efi, escape to fastboot, then continue to recovery.
- Pull `logfs/UefiLog<N>.txt` and `logfs/gbl-chainload_Boot<N>.txt`.
- Assert `UefiLog<N>.txt` contains `qsee-buf | id=` lines (verbose tier landed).
- Assert `gbl-chainload_Boot<N>.txt` does NOT contain `qsee-buf` (verbose-tier did not reach the gbl-chainload file via sink mirror).
- Assert `gbl-chainload_Boot<N>.txt` contains `qsee | id=` (summary tier mirrored pre-handoff or via ConOut path).
- Assert screen during the patched-ABL phase is quiet (no payload hex flooding).

### 6.3 Failure cases

- UART buffer resize allocation fails: Print a screen warning, stay on stock 32 KiB. Verbose-tier content under `--verbose` wraps when the ring fills. Document this as a known degradation; verify on-device that 1 MiB allocation succeeds on canoe before declaring the verbose tier production-ready.
- `SerialBufferReInit` not reachable from a stage-2 EFI app (QCOM-internal symbol): blocks the design. The plan's first task must verify linkage. If it fails, escalate back to design — don't paper over with an ad-hoc replica.

---

## 7. Open questions

### 7.1 1 MiB buffer in low-memory scenarios

`AllocatePagesRuntimeServiceData(256 pages)` on a device with fragmented runtime memory could fail. Probably not on canoe (multiple GiB DDR, but the runtime pool is small). If it fails, gbl-chainload stays on the stock 32 KiB buffer — a Print() warning surfaces this on screen, and verbose-tier under `--verbose` wraps when the ring fills. The plan should size-check before allocating and pick the largest power-of-two that fits; on-device verification will tell us what canoe gives us in practice.

### 7.2 Stock content lost during gbl-chainload's pre-resize window

Between gbl-chainload entry and the `GblResizeUartLogBuffer` call, our own DEBUG emits land in the *old* 32 KiB buffer. The resize copies content forward, but the copy must preserve wrap-around correctly (the buffer may already be full and rolled over by the time gbl-chainload runs). The plan must include explicit tests for this copy path against synthetic full and wrapped buffers.

---

## 8. Trade-offs

1. **Verbose lands in UefiLog<N>.txt, not a dedicated `gbl-chainload_verbose<N>.txt`.** This was a user concern. UefiLog is its own file on its own rotation cycle; mechanically separate. Calling it "co-opting existing pathflows" — which the user explicitly asked for.
2. **1 MiB permanent runtime allocation.** Trivial on canoe.
3. **No post-EBS write of UART buffer ourselves** — we rely on patched-ABL BDS to do it. If patched ABL hard-crashes pre-flush, content is lost. Same as today.
4. **No runtime toggle for verbose.** Compile-time only. Switching scopes requires a re-build + re-stage. Matches the user's "verbose is for opening scope" framing.

---

## 9. Out of scope (deliberately)

- Replacing `WriteLogBufToPartition` with our own implementation. Stock works; we use it.
- Buffering hooks' payload hex in our own RAM and draining at EBS. UART buffer already does this; adding another layer is redundant.
- Per-hook fine-grained `--verbose-qsee` / `--verbose-scm` toggles. Single `--verbose` flag is sufficient.
- A fastboot subcommand to toggle verbose at runtime. Compile-time only.
- Touching `/proc/bootloader_log`. Stays as-is; UefiLog<N>.txt on logfs is the durable record.

---

## 10. Migration / rollout

PR #17 (`feature/cleanup-p1c-log-stream-split`) is still **open**. Two options:

**A. Land v2 on top of #17** — keep #17 as-is (it shipped the sink-retention + EBS callback + `LogFsClose` discipline, which v2 keeps). Open a new feature branch off `main` after #17 merges, implement v2 on it. Cleanest history; PR #17 keeps its independent value.

**B. Roll v2 into #17 before merge** — rebase v2 changes onto the #17 branch and ship as one combined PR. Larger diff to review but matches the actual sequence we figured things out.

Recommendation: A. The bugs that v2 fixes (verbose-tier dead in logfs) were not in #17's scope — #17 was about log stream split, and it correctly achieves that. v2 is a follow-on that uses the QCOM serial path we hadn't analyzed yet.

Implementation order (in a new PR after #17 merges):

1. Verify `SerialBufferReInit` linkage from a stage-2 EFI app. If blocked, stop and revisit design.
2. Add `GblSerialDbgLib` + `SERIAL_DBG` macro + `GblDebugLib` tee to `SerialPortWrite`.
3. Add `GblResizeUartLogBuffer` + wire to `CommonEarlyInit` before first DEBUG emit.
4. Delete `GBL_DBG_LOGFS_ONLY` infrastructure (`LogFsLib.h` bit, DSC PCD widening, all tagging).
5. Re-tag `ProtocolHookLib` per-call traces (summary at `DEBUG_INFO`, payload at `SERIAL_DBG`). Add per-hook `gXxxCallId` counters.
6. Update `tests/052_log_stream_split.sh` per §6.1.
7. Update memory note `logfs_open_across_handoff.md` to reflect the verbose-tier resolution.
8. On-device verify per §6.2.
