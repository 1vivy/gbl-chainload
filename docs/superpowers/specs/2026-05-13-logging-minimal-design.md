# Logging Minimal — Design

**Status:** Draft (2026-05-13)
**Supersedes:** `2026-05-13-logging-v2-serial-tier-design.md` (the SerialIo-tee design, blocked by Task 0 fail).
**Branch:** `feature/logging-minimal-spec` (off `main`).

---

## 1. Goals

1. **Drop the SCREEN_MASK / DebugSink / GBL_DBG_LOGFS_ONLY infrastructure** that PR #17 introduced. It was solving a problem we now know is shaped differently.
2. **Use `Print()` and the standard `DEBUG()` macro** with compile-time tier strips. Verified mechanism: `Print()` already lands in UART log buffer → `\UefiLog<N>.txt` on canoe via the ConOut → ConSplitter → SerialIo → SioPortLib path the BSP wires up.
3. **Compile-time gates only.** `--debug` and `--verbose` strip macro bodies entirely when not set; zero runtime cost.
4. **Keep `UefiLogSaved{0..4}.txt` rotation.** Earlier bootchain doesn't always rotate `UefiLog1.txt` cleanly, so the rotation logic in `LogFsLib/Rotation.c` stays as-is.
5. **No buffer bump.** The investigation in `docs/re/qcom-uart-log-buffer-{investigation,research,probe-plan}.md` + on-device probes turned up a hard QCOM-side block (`SerialBufferReInit` one-shot lock at line 646 of `SerialPortShLibImpl.c`), so we cannot grow the buffer post-XBL. We work within whatever XBL set — currently 64 KiB on canoe (probed and verified via UartProbeA v5).

---

## 2. What we know from the investigation

Captured in the `docs/re/qcom-uart-log-buffer-*` series, with on-device probe results:

- `gEfiInfoBlkHobGuid = {90A49AFD-422F-08AE-9611-E788D3804845}` — HOB body's first 8 bytes are the `UefiInfoBlk*`.
- On canoe (BOOT.MXF.2.5.3-00131-KAANAPALI):
  - `UefiInfoBlk @ 0xC6E01000`, signature `0x6B6C4249` ('IBlk'), version `0x00010010`.
  - `UartLogBufferPtr = 0x81CE4000`, `UartLogBufferLen = 0x10000` (64 KiB).
- `SerialBufferReInit` locks itself on first call (XBL invokes it once). Subsequent calls return `EFI_UNSUPPORTED` — so we cannot redirect SioPortLib's cached buffer pointer post-XBL.
- `Print()` calls on canoe land in `\UefiLog<N>.txt` (user-observed) — confirms ConOut → splitter → UART buffer wiring is alive at our stage.
- `gEfiSerialIoProtocol` is **not** installed at our stage-2 EFI app level (verified by Task 0's probe). The patched ABL reaches the UART buffer via QCOM's statically-linked SerialPortShLib, not via this protocol — we cannot mimic that path without proprietary source we can't vendor.

The simplification falls out of these facts: since `Print()` is the only viable path we have to UART/UefiLog, route everything through it.

---

## 3. Tier definitions

| Macro / call | Build flag | Resolves to | Screen? | UefiLog? |
|--------------|------------|-------------|---------|----------|
| `Print(L"...")` | always | `gST->ConOut->OutputString` | yes | yes |
| `DEBUG((DEBUG_ERROR, ...))` | always | standard EDK2 DebugLib → ConOut | yes | yes |
| `GBL_INFO("...")` | `GBL_DEBUG=0` | `DEBUG((DEBUG_INFO, ...))` (efi_debug path) | no (silent per canoe observation) | yes |
| `GBL_INFO("...")` | `GBL_DEBUG=1` (`--debug`) | `AsciiPrint(...)` (ConOut path) | yes | yes |
| `VERBOSE("...")` | `GBL_VERBOSE=0` | NO-OP (compile-stripped) | — | — |
| `VERBOSE("...")` | `GBL_VERBOSE=1` (`--verbose`) | `AsciiPrint(...)` (ConOut path) | yes | yes |

**Same UefiLog destination either way.** The `GBL_INFO` macro picks between two paths to that destination based on whether the line should ALSO be on screen. Single emit per call site — no duplicate-in-UefiLog issue, because each build uses exactly one of the two paths per call site.

The "silent on canoe under `GBL_DEBUG=0`" property is observed platform behavior for the EDK2 `DEBUG()` call, not a design guarantee. If on-device testing later shows DEBUG content IS noisy on screen under prod builds, revisit by either (a) re-mapping DebugLib to a serial-only variant, or (b) introducing a thin screen-mask. For now we proceed on the observation.

All output destinations downstream of either path:
- Screen (framebuffer console; on canoe goes quiet post-handoff per BSP behaviour).
- UART buffer at `0x81CE4000` (via the QCOM ConOut/splitter/SioPortLib chain).
- Eventually flushed to `\UefiLog<N>.txt` by stock BDS at pre-boot.

Nothing else. No DebugSink, no logfs mirror, no screen mask, no GBL_DBG_LOGFS_ONLY, no direct UART buffer writes, no SerialBufferReInit gymnastics.

---

## 4. Components

### 4.1 `GblDebugLib` — replaced with header-only macros

Delete the existing `GblChainloadPkg/Library/GblDebugLib/` library entirely. Replace with a single public header:

`GblChainloadPkg/Include/Library/GblLog.h`

```c
/** @file GblLog.h — minimal logging API for gbl-chainload.

  All emits ultimately land in \UefiLog<N>.txt — either via the
  EDK2 DEBUG() macro (efi_debug → standard DebugLib → UefiLog) or
  via AsciiPrint() (ConOut path → screen + UefiLog).

  GBL_INFO selects between the two at build time:
    GBL_DEBUG=0: GBL_INFO expands to DEBUG((DEBUG_INFO, ...))
                 — silent log (via efi_debug)
    GBL_DEBUG=1: GBL_INFO expands to AsciiPrint(...)
                 — visible on screen + in UefiLog

  Both paths reach UefiLog. Picking between them is purely about
  screen visibility. Single emit per call site — no duplicates.

  VERBOSE compile-strips entirely when GBL_VERBOSE=0 (zero code at
  call sites). Under GBL_VERBOSE=1, expands to AsciiPrint() so the
  verbose hex dumps from hooks are visible and logged.

  Call sites use CHAR8 ASCII format strings ("foo=%u\n"), not
  L-prefixed Unicode literals. AsciiPrint widens internally for
  ConOut; DEBUG takes ASCII natively.
**/

#ifndef GBL_LOG_H_
#define GBL_LOG_H_

#include <Library/UefiLib.h>      /* AsciiPrint, Print */
#include <Library/DebugLib.h>     /* DEBUG, DEBUG_ERROR, DEBUG_INFO */

#ifndef GBL_DEBUG
# define GBL_DEBUG 0
#endif
#ifndef GBL_VERBOSE
# define GBL_VERBOSE 0
#endif

/* Always-on errors. Use Print(L"...") directly at call sites — no
 * macro wrap. Print is the only thing that must be visible
 * unconditionally (failures, user prompts). */

/* Debug-tier: swap mechanism between DEBUG (silent log) and AsciiPrint
 * (visible + log). Same destination either way; difference is screen
 * visibility. */
#if (GBL_DEBUG == 1)
# define GBL_INFO(fmt, ...)   AsciiPrint (fmt, ##__VA_ARGS__)
#else
# define GBL_INFO(fmt, ...)   DEBUG ((DEBUG_INFO, fmt, ##__VA_ARGS__))
#endif

/* Verbose-tier: hard compile-strip in non-verbose builds. Zero code
 * at call sites; format strings absent from .rodata. */
#if (GBL_VERBOSE == 1)
# define VERBOSE(fmt, ...)    AsciiPrint (fmt, ##__VA_ARGS__)
#else
# define VERBOSE(fmt, ...)    do { (void)0; } while (0)
#endif

#endif /* GBL_LOG_H_ */
```

**Why this works without a screen mask:** under `GBL_DEBUG=0`, `GBL_INFO` resolves to standard `DEBUG((DEBUG_INFO, ...))`. EDK2's DebugLib routes that to the platform DebugLib instance (currently `UefiDebugLibConOut`), which writes via `gST->ConOut->OutputString`. On canoe specifically, observation has it that DEBUG content reaches UefiLog without dominating the framebuffer the way `Print()` calls do — that's the assumption baked into this design. If on-device testing later contradicts this (DEBUG output IS noisy on screen under prod builds), we revisit by either swapping to `UefiDebugLibSerialPort` with a non-null SerialPortLib, or by re-introducing a thin mask. Documenting the assumption explicitly here so future-us doesn't forget.

**Call sites that currently use `DEBUG((DEBUG_INFO, "..."))` get bulk-renamed to `GBL_INFO("...")`.** `DEBUG((DEBUG_ERROR, "..."))` and explicit `Print(L"...")` stay as-is. Verbose tier currently expressed as `DEBUG((GBL_DBG_LOGFS_ONLY, ...))` in PR #17 gets bulk-renamed to `VERBOSE("...")`.

### 4.2 `LogFsLib` — reduced

Keep these files unchanged in function (light cleanup OK):
- `Mount.c` — locate `logfs` partition, mount, open `gbl-chainload_Boot<N>.txt`.
- `Rotation.c` — rotate `UefiLog1.txt` → `UefiLogSaved{0..4}.txt`. **This is the "earlier bootchain doesn't rotate properly" workaround you specifically called out.**
- `PostGblLog.c` — append-write to `gbl-chainload_Boot<N>.txt`.

**Delete entirely:**
- `DebugSink.c` — the `HookedOutputString` machinery, `gGblScreenMask`, `LogFsInstallDebugSink`, `LogFsRemoveDebugSink`. Public-header entries for those four functions go too.
- The EBS callback registered in `LogFsInit` — not needed once we're not buffering writes asynchronously.

Public `LogFsLib.h` shrinks to: `LogFsInit`, `LogFsWrite`, `LogFsWriteBlob`, `LogFsFlush`, `LogFsClose`, `LogFsIsReady`. Six functions, all explicit-call.

### 4.3 `Entry.c` and `BootFlow.c` — explicit dual emit where wanted

Pattern for status lines that should land in BOTH `\UefiLog<N>.txt` and `gbl-chainload_Boot<N>.txt`:

```c
Print (L"BootFlow: patches applied=%u missed=%u\n", Applied, Missed);
LogFsWrite (Buf, AsciiSPrint (Buf, sizeof (Buf),
            "BootFlow: patches applied=%u missed=%u\n", Applied, Missed));
```

Two calls. Yes, slightly verbose. The user explicitly chose this over the auto-mirror (DebugSink) approach.

Where automatic dual emit IS wanted (the common case), introduce one helper at most:

```c
/* In Entry.c or a small util header: */
STATIC VOID
GblPrintAndLog (
  IN CONST CHAR16 *Fmt,
  ...
  )
{
  CHAR16   WideBuf[256];
  CHAR8    AsciiBuf[256];
  VA_LIST  Marker;
  UINTN    Len;

  VA_START (Marker, Fmt);
  UnicodeVSPrint (WideBuf, sizeof (WideBuf), Fmt, Marker);
  VA_END (Marker);

  Print (L"%s", WideBuf);

  Len = UnicodeStrToAsciiStrS (WideBuf, AsciiBuf, sizeof (AsciiBuf));
  if (Len < sizeof (AsciiBuf)) {
    LogFsWrite (AsciiBuf, AsciiStrLen (AsciiBuf));
  }
}
```

This helper is local to `Application/GblChainload/`. Hooks in `ProtocolHookLib/*Hook.c` do NOT use it — they're post-handoff (logfs closed) and emit via plain `Print()` only.

### 4.4 `BootFlow.c` discipline (unchanged)

- `LogFsClose()` BEFORE `gBS->LoadImage` — partition handle release rule. Stays. This is the durable invariant from `docs/superpowers/specs/2026-05-12-cleanup-phase-1-design.md` and the on-device-verified `logfs_open_across_handoff` memory.

### 4.5 `ProtocolHookLib` — `Print()` for hook output

All hook-emit calls in `Qsee/Scm/VerifiedBoot/Spss/Mode1Overlay` files switch to:

- One-line summary per hook entry: `Print(L"qsee | id=%u | cmd=0x%x | ...\n", ...)` if we want it visible, OR `GBL_DEBUG_INFO("qsee | ...")` if --debug-gated.
- Payload hex dumps: `VERBOSE("qsee-buf | id=%u | dir=s | off=%u | hex=%a\n", ...)` — stripped in non-verbose builds.

Per-hook static counter `gQseeCallId` etc. (same pattern as the v1 plan).

Remove all `DEBUG((GBL_DBG_LOGFS_ONLY, ...))` calls and the `GBL_DBG_LOGFS_ONLY` define itself.

### 4.6 DSC changes

- Drop the `GBL_VERBOSE`-conditional `PcdDebugPrintErrorLevel` widening (revert to unconditional `0x80000042`).
- Drop the `DebugLib|GblChainloadPkg/Library/GblDebugLib/GblDebugLib.inf` mapping; revert to `UefiDebugLibConOut`.
- Drop `LogFsLib`-related entries in the `[Components.common]` overrides for `GblChainload.inf` that pulled in `DebugSink.c`-specific deps (most stay, just the now-irrelevant ones go).
- Remove the `GblDebugLib` from component overrides; the global mapping covers everything.

### 4.7 Build flags / scripts

`scripts/build.sh` and `scripts/build-inside-docker.sh`: keep `--debug` and `--verbose` flags as-is. They set `GBL_DEBUG=1` / `GBL_VERBOSE=1` environment vars which the DSC passes through as `-D` to CC.

Build artifact names: `mode-1.efi`, `mode-1-debug.efi`, `mode-1-debug-verbose.efi`, plus `-auto` variants. Same as today.

---

## 5. Tests

### 5.1 `tests/052_log_stream_split.sh` — rewrite

Drop most of the existing checks (they assert the dropped infrastructure). New checks:

```bash
# Check 1: GBL_DEBUG_INFO macro defined and compile-gated.
HDR=GblChainloadPkg/Include/Library/GblLog.h
[ -f "$HDR" ] || fail "GblLog.h missing"
grep -q '#if (GBL_DEBUG == 1)' "$HDR" || fail "GBL_DEBUG_INFO not compile-gated"
grep -q '#if (GBL_VERBOSE == 1)' "$HDR" || fail "VERBOSE not compile-gated"

# Check 2: GBL_DBG_LOGFS_ONLY references fully removed.
if grep -rn 'GBL_DBG_LOGFS_ONLY' GblChainloadPkg 2>/dev/null | grep -v '^Binary'; then
  fail "GBL_DBG_LOGFS_ONLY references still present"
fi

# Check 3: DebugSink.c and related deleted.
[ ! -f GblChainloadPkg/Library/LogFsLib/DebugSink.c ] || fail "DebugSink.c not deleted"
[ ! -f GblChainloadPkg/Library/GblDebugLib/GblDebugLib.c ] || fail "GblDebugLib not deleted"

# Check 4: ProtocolHookLib uses Print/GBL_DEBUG_INFO/VERBOSE only (no DEBUG_INFO / DEBUG with logfs-only level).
if grep -rn 'GBL_DBG_LOGFS_ONLY\|HookedOutputString' GblChainloadPkg/Library/ProtocolHookLib 2>/dev/null; then
  fail "ProtocolHookLib still has old logging plumbing"
fi

# Check 5: LogFsClose() before LoadImage discipline.
BF=GblChainloadPkg/Application/GblChainload/BootFlow.c
if ! awk '/gBS->LoadImage/{exit} {print}' "$BF" | grep -q 'LogFsClose'; then
  fail "BootFlow.c: LogFsClose() must be called before LoadImage"
fi
```

Rename file if appropriate (`052_log_minimal.sh` or similar).

### 5.2 `tests/010_build_smoke.sh` — add strip verification

```bash
# VERBOSE() body absent in non-verbose builds.
if strings dist/mode-1-debug.efi | grep -q 'qsee-buf | id='; then
  fail "verbose-tier hex format string present in non-verbose build"
fi

# VERBOSE() body PRESENT in verbose builds.
if ! strings dist/mode-1-debug-verbose.efi | grep -q 'qsee-buf | id='; then
  warn "verbose-tier format string not visible via strings (compile may have stripped string literals; check via nm)"
fi
```

### 5.3 On-device verification

User runs the build + stage cycle. Three variants:

- **prod (`mode-1.efi`)**: silent screen, errors only via `Print()`. `UefiLog<N>.txt` contains stock + ABL DEBUG content. `gbl-chainload_Boot<N>.txt` contains the explicit-LogFsWrite content from `BootFlow.c` (patch outcomes, hook install summary).
- **debug (`mode-1-debug.efi`)**: screen shows debug-tier under `--debug`. `UefiLog<N>.txt` has debug-tier emits too. No hex dumps.
- **debug + verbose (`mode-1-debug-verbose.efi`)**: screen shows debug-tier. `UefiLog<N>.txt` has debug-tier + hook hex dumps. May wrap on long runs — accepted.

---

## 6. Migration / rollout

PR #17 (`feature/cleanup-p1c-log-stream-split`) is still open. Two options:

**A. Rebase #17 to drop its DebugSink-era changes, then layer the minimal design on top.**
- Cleanest history (one PR represents one design).
- Larger diff to review.

**B. Close #17 without merge. Land the minimal design as its own PR cut from `main`.**
- #17's design ended up wrong; closing acknowledges that.
- Some useful work from #17 (UefiLogSaved rotation discipline; the `LogFsClose` before `LoadImage` fix) needs to be replicated.

**Recommendation: B.** The minimal design is small enough that recreating the load-bearing pieces of #17 (rotation + close discipline) inside its branch is easier than rebasing.

---

## 7. What this design explicitly drops

- `GblDebugLib` — entire library.
- `DebugSink.c` — entire file.
- `HookedOutputString` — function and concept.
- `gGblScreenMask` and `LogFsSetScreenMask` — variable and API.
- `gDbgCurrentLevel` — extern shared between GblDebugLib and DebugSink.
- `GBL_DBG_LOGFS_ONLY` — error-level bit and all usage.
- `--verbose`-conditional `PcdDebugPrintErrorLevel` widening — revert to unconditional.
- LogFsLib's EBS callback — `LogFsExitBootServicesNotify`.

That's roughly **400-500 lines of LogFsLib + GblDebugLib code** disappearing, replaced by **one header file** and a couple of explicit `Print()`/`LogFsWrite()` calls in `BootFlow.c`.

---

## 8. Trade-offs

1. **Verbose hex dumps wrap on long runs.** Buffer is 64 KiB. Stock + ABL fill ~30–40 KiB of debug-tier content per boot. Verbose adds more. On a long verbose run UefiLog wraps. Accepted; we don't have headroom and the QCOM lock makes the bump impossible. If long verbose captures are needed, the proper fix is a custom uefiplat.cfg with `UARTLogBufferSize=0x40000` flashed to EFISP — a one-time setup step outside this design.

2. **`gbl-chainload_Boot<N>.txt` is now sparse.** It only contains what `BootFlow.c` explicitly mirrors via `LogFsWrite`. Hook content goes only to UefiLog. If you want gbl-chainload-internal content to also be in the logfs file, add explicit `LogFsWrite` calls — but for hook content, that file is silent under this design.

3. **No async flush guarantee for `gbl-chainload_Boot<N>.txt`.** Under #17, the EBS callback did a final flush. Under this design, callers must call `LogFsFlush()` themselves before risky operations. `BootFlow.c` already does this at several key points; we keep those.

4. **`Print()` from a hook running inside the patched ABL relies on `gST` still being valid.** This is a UEFI guarantee until `ExitBootServices`. Hooks fire before EBS, so they're fine. After EBS, anything that emits is dropped (`Print()` returns harmlessly).

---

## 9. Out of scope

- The QCOM UART buffer bump — provably blocked by `SerialBufferReInit` lock at line 646 of SerialPortShLibImpl.c.
- Any fastboot OEM command to inspect / manipulate the buffer.
- Capturing post-EBS content from the patched ABL (impossible without EBS hook tricks we aren't doing).
- A custom uefiplat.cfg re-flash to raise the buffer ceiling — a separate work item if/when we get there.
- Modifications to the patched-ABL static linkage (would require vendoring proprietary QCOM source).
