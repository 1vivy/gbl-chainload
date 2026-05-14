# Logging Minimal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the PR #17 logging design (DebugSink with screen mask + GBL_DBG_LOGFS_ONLY infrastructure, plus a GblDebugLib shim) with a header-only compile-time-gated macro set built on standard EDK2 `DEBUG()` and `AsciiPrint()`. Land as a fresh PR cut from `main` — does not depend on #17.

**Architecture:** Build a one-header logging API (`GblLog.h`) that exposes two macros: `GBL_INFO(asciifmt, ...)` swaps between `DEBUG((DEBUG_INFO, ...))` (silent under `GBL_DEBUG=0`) and `AsciiPrint(...)` (visible under `GBL_DEBUG=1`); `VERBOSE(asciifmt, ...)` compile-strips to nothing under `GBL_VERBOSE=0` and expands to `AsciiPrint(...)` under `=1`. Same UefiLog destination either way; difference is screen visibility. Single emit per call site, no duplicate-in-UefiLog issue.

**Tech Stack:** EDK2 (AArch64, CLANG35), `MdePkg/Library/UefiDebugLibConOut`, `MdePkg/Library/UefiLib` (`AsciiPrint`), our existing `LogFsLib` (Mount/Rotation/PostGblLog/DebugSink). Standard `DEBUG()` macro lands in `\UefiLog<N>.txt` on canoe via the QCOM ConSplitter→SerialIo→SioPortLib chain.

**Branch:** `feature/logging-minimal-impl` cut from `main`. Spec lives at `docs/superpowers/specs/2026-05-13-logging-minimal-design.md` on `feature/logging-minimal-spec`.

**Important baseline note:** `main` already has most prerequisites. The DSC defines `GBL_DEBUG`/`GBL_VERBOSE` (defaults to 0), passes them as `-D` CC flags, maps `DebugLib` to `UefiDebugLibConOut`, and sets `PcdDebugPrintErrorLevel` to unconditional `0x80000042`. `LogFsLib/DebugSink.c` exists on main as a simple ConOut→logfs mirror (no screen mask, no level gating — just a transparent mirror). The plan keeps that DebugSink as-is — it provides the auto-mirror of Print/DEBUG into `gbl-chainload_Boot<N>.txt`, which is useful and doesn't conflict with the new macro design.

**What the plan actually does:**
1. Verify `main` is at the assumed baseline.
2. Add `GblLog.h` with the two macros.
3. Update call sites in `Application/GblChainload/` and `Library/ProtocolHookLib/` and `Library/AblUnwrapLib/` to use `GBL_INFO` / `VERBOSE` where they currently use `DEBUG((DEBUG_INFO, ...))` or `DEBUG((DEBUG_VERBOSE, ...))`.
4. Rewrite `tests/052_log_stream_split.sh` as `tests/052_log_minimal.sh`.
5. Extend `tests/010_build_smoke.sh` with strip-verification.
6. Hand the on-device verification to the user.

---

## File Structure

**Created:**

| Path | Responsibility |
|------|----------------|
| `GblChainloadPkg/Include/Library/GblLog.h` | Single header defining `GBL_INFO` and `VERBOSE` macros. ~50 lines. |

**Modified:**

| Path | Change |
|------|--------|
| `GblChainloadPkg/Application/GblChainload/Entry.c` | Add `#include <Library/GblLog.h>`. Replace any `DEBUG((DEBUG_INFO, "..."))` with `GBL_INFO("...")`. |
| `GblChainloadPkg/Application/GblChainload/BootFlow.c` | Same. |
| `GblChainloadPkg/Library/AblUnwrapLib/AblUnwrapLib.c` | Same. |
| `GblChainloadPkg/Library/ProtocolHookLib/QseecomHook.c` | Add include + rename DEBUG_INFO → GBL_INFO + add VERBOSE for any high-volume hex emits. |
| `GblChainloadPkg/Library/ProtocolHookLib/ScmHook.c` | Same. |
| `GblChainloadPkg/Library/ProtocolHookLib/VerifiedBootHook.c` | Same. |
| `GblChainloadPkg/Library/ProtocolHookLib/SpssHook.c` | Same. |
| `GblChainloadPkg/Library/ProtocolHookLib/Mode1Overlay.c` | Same. |
| `GblChainloadPkg/Library/ProtocolHookLib/UniversalBaseline.c` | Same. |
| `tests/052_log_stream_split.sh` | Renamed and rewritten as `tests/052_log_minimal.sh`. |
| `tests/010_build_smoke.sh` | Add strip-verification at the tail. |

**Not changed:**

| Path | Why |
|------|-----|
| `GblChainloadPkg/GblChainloadPkg.dsc` | Already has `GBL_DEBUG`/`GBL_VERBOSE` defines + CC-flag plumbing + correct DebugLib + correct PCD. |
| `GblChainloadPkg/Library/LogFsLib/*.c` | DebugSink is the auto-mirror; Mount/Rotation/PostGblLog are the file mechanics. All useful and minimal. |
| `GblChainloadPkg/Include/Library/LogFsLib.h` | Public API (Init/Write/WriteBlob/Flush/Close/IsReady/InstallDebugSink/RemoveDebugSink) stays. |
| `scripts/build.sh`, `scripts/build-inside-docker.sh` | Already handle `--debug` / `--verbose` flags and propagate `GBL_DEBUG` / `GBL_VERBOSE` env vars to the build. |

---

## Task 0: Verify baseline state

**Files:** none modified. Just checks.

**Purpose:** Confirm `main` is at the expected baseline before starting any code changes. If any of these checks fail, the plan needs adjustment.

- [ ] **Step 1: Cut the branch and verify checkout**

```bash
cd /home/vivy/gbl-chainload
git checkout main
git pull origin main
git checkout -b feature/logging-minimal-impl
git log --oneline -1
```

Expected: latest commit is `c5cce89 Merge pull request #14 from 1vivy/feature/cleanup-phase-1-design-v2` or newer.

- [ ] **Step 2: Verify DSC baseline**

```bash
grep -E 'GBL_DEBUG\s*=|GBL_VERBOSE\s*=|UefiDebugLibConOut|PcdDebugPrintErrorLevel' GblChainloadPkg/GblChainloadPkg.dsc
```

Expected output includes:
- `DEFINE GBL_DEBUG                        = 0`
- `DEFINE GBL_VERBOSE                      = 0`
- `DebugLib|MdePkg/Library/UefiDebugLibConOut/UefiDebugLibConOut.inf`
- `gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000042`

If any line is missing or different, STOP and surface to the user — the plan assumes these.

- [ ] **Step 3: Verify build-flag passthrough in DSC**

```bash
grep -E 'GBL_DEBUG=\$\(GBL_DEBUG\)|GBL_VERBOSE=\$\(GBL_VERBOSE\)' GblChainloadPkg/GblChainloadPkg.dsc
```

Expected: both lines present in `[BuildOptions.common]`.

- [ ] **Step 4: Verify LogFsLib's DebugSink is the simple version**

```bash
grep -c 'gGblScreenMask\|gDbgCurrentLevel\|GBL_DBG_LOGFS_ONLY' GblChainloadPkg/Library/LogFsLib/DebugSink.c
```

Expected: `0`. If any of those tokens appear, the plan's "DebugSink stays as-is" assumption is wrong — we'd be looking at PR #17's elaborated version and need to revisit.

- [ ] **Step 5: Verify build script flag plumbing**

```bash
grep -E '\bGBL_DEBUG\b|\bGBL_VERBOSE\b|--debug\b|--verbose\b' scripts/build.sh
```

Expected to show `--debug` and `--verbose` flags handled, with `GBL_DEBUG` and `GBL_VERBOSE` env vars set.

- [ ] **Step 6: Run baseline build to confirm main builds cleanly**

```bash
bash scripts/build.sh --mode 1 --auto 2>&1 | tail -5
```

Expected last line: `==> Built dist/mode-1-auto.efi (...)`. If it doesn't build, the entire plan can't proceed.

- [ ] **Step 7: Commit nothing — proceed to Task 1**

No code changes yet. If all checks passed, the baseline is confirmed.

---

## Task 1: Create `GblLog.h`

**Files:**
- Create: `GblChainloadPkg/Include/Library/GblLog.h`

- [ ] **Step 1: Write the header**

```c
/** @file GblLog.h — minimal logging API for gbl-chainload.

  Two compile-time-gated macros, both with ASCII format strings:

    GBL_INFO(fmt, ...) — debug-tier emit.
      GBL_DEBUG=0: DEBUG((DEBUG_INFO, ...))  — silent log via efi_debug
      GBL_DEBUG=1: AsciiPrint(...)           — visible + log

    VERBOSE(fmt, ...) — verbose-tier emit.
      GBL_VERBOSE=0: NO-OP (compile-stripped)
      GBL_VERBOSE=1: AsciiPrint(...)         — visible + log

  Both paths reach \UefiLog<N>.txt — DEBUG via the EDK2 DebugLib's
  ConOut path (UefiDebugLibConOut on canoe); AsciiPrint via UEFI's
  Print mechanism (also ConOut). The "silent" property of GBL_INFO
  under GBL_DEBUG=0 is observed canoe behavior of DEBUG-level emits
  not dominating the framebuffer console. Documented in the design
  spec §4.1.

  All format strings are CHAR8 ASCII ("foo=%u\n"), NOT L-prefixed.
  AsciiPrint widens internally for ConOut; DEBUG takes ASCII natively.
**/

#ifndef GBL_LOG_H_
#define GBL_LOG_H_

#include <Library/UefiLib.h>    /* AsciiPrint, Print */
#include <Library/DebugLib.h>   /* DEBUG, DEBUG_INFO, DEBUG_ERROR */

#ifndef GBL_DEBUG
# define GBL_DEBUG 0
#endif
#ifndef GBL_VERBOSE
# define GBL_VERBOSE 0
#endif

/* Debug-tier: swap mechanism between DEBUG (silent) and AsciiPrint
 * (visible). Same UefiLog destination either way. */
#if (GBL_DEBUG == 1)
# define GBL_INFO(fmt, ...)   AsciiPrint (fmt, ##__VA_ARGS__)
#else
# define GBL_INFO(fmt, ...)   DEBUG ((DEBUG_INFO, fmt, ##__VA_ARGS__))
#endif

/* Verbose-tier: hard compile-strip in non-verbose builds. */
#if (GBL_VERBOSE == 1)
# define VERBOSE(fmt, ...)    AsciiPrint (fmt, ##__VA_ARGS__)
#else
# define VERBOSE(fmt, ...)    do { (void)0; } while (0)
#endif

#endif /* GBL_LOG_H_ */
```

- [ ] **Step 2: Build smoke (no consumers yet, just header existence + parse)**

```bash
bash scripts/build.sh --mode 1 --auto 2>&1 | tail -3
```

Expected: build still succeeds. Header isn't `#include`d anywhere so no compile change yet.

- [ ] **Step 3: Commit**

```bash
git add GblChainloadPkg/Include/Library/GblLog.h
git commit -m "$(cat <<'EOF'
GblLog.h: GBL_INFO and VERBOSE macros for compile-time-gated emit

GBL_INFO swaps between DEBUG((DEBUG_INFO, ...)) under GBL_DEBUG=0
(silent on canoe per observation; lands in UefiLog via efi_debug)
and AsciiPrint() under GBL_DEBUG=1 (visible on screen + UefiLog).
Same destination, single emit per call site.

VERBOSE hard compile-strips when GBL_VERBOSE=0; expands to
AsciiPrint() when =1.

Source spec: docs/superpowers/specs/2026-05-13-logging-minimal-design.md

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Adopt `GBL_INFO` in `Entry.c`

**Files:**
- Modify: `GblChainloadPkg/Application/GblChainload/Entry.c`

This is the smallest call-site update; do it first to validate the include path and macro behavior end-to-end.

- [ ] **Step 1: Survey `Entry.c`'s current DEBUG calls**

```bash
grep -n 'DEBUG ((' GblChainloadPkg/Application/GblChainload/Entry.c
```

Note the line numbers and levels (`DEBUG_ERROR`, `DEBUG_INFO`, `DEBUG_WARN`). Only `DEBUG_INFO` lines get renamed.

- [ ] **Step 2: Add the include**

Open `GblChainloadPkg/Application/GblChainload/Entry.c`. After the existing `#include <Library/DebugLib.h>` line (or after the last existing `#include` if `DebugLib.h` isn't there), add:

```c
#include <Library/GblLog.h>
```

- [ ] **Step 3: Rename `DEBUG((DEBUG_INFO, ...))` → `GBL_INFO(...)`**

For each line matching `DEBUG ((DEBUG_INFO, "<fmt>", <args>));`, rewrite as:

```c
GBL_INFO ("<fmt>", <args>);
```

Leave `DEBUG((DEBUG_ERROR, ...))` and `DEBUG((DEBUG_WARN, ...))` alone — those don't get the swap-mechanism treatment.

- [ ] **Step 4: Build smoke**

```bash
bash scripts/build.sh --mode 1 --auto 2>&1 | tail -3
```

Expected: succeeds. If `AsciiPrint` is unresolved at link time, `UefiLib` is the library class to add to GblChainload.inf (verify with `grep UefiLib GblChainloadPkg/Application/GblChainload/GblChainload.inf` — it should already be there since `Print()` is used).

- [ ] **Step 5: Build with `--debug` and verify GBL_INFO routes through AsciiPrint**

```bash
bash scripts/build.sh --mode 1 --auto --debug 2>&1 | tail -3
```

Expected: succeeds. The AsciiPrint code path is exercised.

- [ ] **Step 6: Commit**

```bash
git add GblChainloadPkg/Application/GblChainload/Entry.c
git commit -m "Entry.c: adopt GBL_INFO for DEBUG_INFO emits

Replaces DEBUG((DEBUG_INFO, ...)) with GBL_INFO(...) so the
prod/debug split is compile-time-gated per the minimal-logging
design. No behavior change in prod builds (still goes through
efi_debug); --debug builds now route through AsciiPrint for
explicit screen visibility.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Adopt `GBL_INFO` in `BootFlow.c`

**Files:**
- Modify: `GblChainloadPkg/Application/GblChainload/BootFlow.c`

Same shape as Task 2. BootFlow.c has more DEBUG_INFO lines (boot flow has many status updates).

- [ ] **Step 1: Survey**

```bash
grep -n 'DEBUG ((' GblChainloadPkg/Application/GblChainload/BootFlow.c
```

- [ ] **Step 2: Add the include**

After the existing `#include <Library/DebugLib.h>` in BootFlow.c, add:

```c
#include <Library/GblLog.h>
```

- [ ] **Step 3: Rename DEBUG_INFO emits**

For each line `DEBUG ((DEBUG_INFO, "fmt\n", args));` rewrite as `GBL_INFO ("fmt\n", args);`. Preserve trailing newlines exactly as in the original. Leave DEBUG_ERROR / DEBUG_WARN lines alone.

- [ ] **Step 4: Build smoke (default + --debug)**

```bash
bash scripts/build.sh --mode 1 --auto && bash scripts/build.sh --mode 1 --auto --debug
```

Expected: both succeed.

- [ ] **Step 5: Commit**

```bash
git add GblChainloadPkg/Application/GblChainload/BootFlow.c
git commit -m "BootFlow.c: adopt GBL_INFO for DEBUG_INFO emits

Same pattern as Entry.c. Patch outcomes, hook install summary,
chainload handoff progress all become silent-log under prod and
screen-visible under --debug.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Adopt `GBL_INFO` in `AblUnwrapLib.c`

**Files:**
- Modify: `GblChainloadPkg/Library/AblUnwrapLib/AblUnwrapLib.c`

AblUnwrapLib has both short summary emits and high-volume scan-loop emits. Short ones become `GBL_INFO`; scan-loop ones become `VERBOSE` (so they compile-strip in non-verbose builds and don't bloat the binary).

- [ ] **Step 1: Survey + classify**

```bash
grep -n 'DEBUG ((' GblChainloadPkg/Library/AblUnwrapLib/AblUnwrapLib.c
```

Read each match and classify by line content:
- Single-emit-per-call summary lines ("matched abl_a", "FV @ offset X size Y", "found PE/TE N bytes", "PE/TE size 0xY"): become `GBL_INFO`.
- Per-section scan-loop emits ("section @ 0xX type=0xY size=0xZ hdr=N", "GUID_DEFINED off=X inner=Y", "FV_IMAGE section, scanning N bytes"): become `VERBOSE`.

- [ ] **Step 2: Add the include**

After the existing `#include <Library/DebugLib.h>` in AblUnwrapLib.c, add:

```c
#include <Library/GblLog.h>
```

- [ ] **Step 3: Apply renames**

For each classified line, rewrite:
- Summary: `DEBUG ((DEBUG_INFO, "..."));` → `GBL_INFO ("...");`
- Scan-loop: `DEBUG ((DEBUG_INFO, "..."));` → `VERBOSE ("...");`

Preserve the format string and arguments exactly. If a line uses `DEBUG_VERBOSE` or another level (unlikely on main), leave it.

- [ ] **Step 4: Build smoke (three variants)**

```bash
bash scripts/build.sh --mode 1 --auto
bash scripts/build.sh --mode 1 --auto --debug
bash scripts/build.sh --mode 1 --auto --debug --verbose
```

All three should succeed.

- [ ] **Step 5: Quick strip verification**

```bash
strings dist/mode-1-auto.efi          | grep -c 'section @ 0x'
strings dist/mode-1-auto-debug.efi    | grep -c 'section @ 0x'
strings dist/mode-1-auto-debug-verbose.efi | grep -c 'section @ 0x'
```

Expected:
- `mode-1-auto.efi`: 0 (VERBOSE stripped, format string absent).
- `mode-1-auto-debug.efi`: 0 (same — only `--verbose` enables VERBOSE).
- `mode-1-auto-debug-verbose.efi`: ≥ 1 (VERBOSE body present, format string in .rodata).

If `mode-1-auto.efi` shows ≥ 1, the compile-strip isn't working — VERBOSE isn't actually expanding to `do {} while (0)`. Investigate before proceeding.

- [ ] **Step 6: Commit**

```bash
git add GblChainloadPkg/Library/AblUnwrapLib/AblUnwrapLib.c
git commit -m "AblUnwrapLib: GBL_INFO for summary, VERBOSE for scan-loop emits

Summary lines (matched abl_a, found PE/TE, etc.) become GBL_INFO so
they're silent-log under prod and visible under --debug. Scan-loop
per-section emits become VERBOSE — compile-stripped in non-verbose
builds so they don't bloat the binary.

Strip verified: 'section @ 0x' format string absent from
mode-1-auto.efi and mode-1-auto-debug.efi; present in
mode-1-auto-debug-verbose.efi.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Adopt `GBL_INFO`/`VERBOSE` in `QseecomHook.c`

**Files:**
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/QseecomHook.c`

QseecomHook has hook entry-point emits (one summary line per hook invocation — "qsee | cmd=X | ...") and payload hex emits (`qsee-buf | dir=s | off=N | hex=Y`). Summary → GBL_INFO; payload hex → VERBOSE.

- [ ] **Step 1: Survey**

```bash
grep -n 'DEBUG ((' GblChainloadPkg/Library/ProtocolHookLib/QseecomHook.c
```

- [ ] **Step 2: Add include**

After existing `#include <Library/DebugLib.h>`:

```c
#include <Library/GblLog.h>
```

- [ ] **Step 3: Apply renames**

For each `DEBUG ((DEBUG_INFO, "qsee | ...", ...));` (summary line, one per hook entry): `GBL_INFO ("qsee | ...", ...);`

For each `DEBUG ((DEBUG_INFO, "qsee-buf | ...", ...));` (hex dump): `VERBOSE ("qsee-buf | ...", ...);`

If main's QseecomHook.c doesn't have these specific patterns (it's prior to the PR #17 retag), apply the classification based on the actual content: any line that emits in a loop or with hex data → VERBOSE; one-line summaries → GBL_INFO.

- [ ] **Step 4: Build smoke**

```bash
bash scripts/build.sh --mode 1 --auto --debug --verbose 2>&1 | tail -3
```

Expected: succeeds.

- [ ] **Step 5: Commit**

```bash
git add GblChainloadPkg/Library/ProtocolHookLib/QseecomHook.c
git commit -m "QseecomHook: GBL_INFO summary, VERBOSE payload hex

qsee | ... one-liners use GBL_INFO so they appear silently in
UefiLog under prod and visible on screen under --debug.
qsee-buf | ... payload hex dumps use VERBOSE — only present
under --verbose builds.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: Adopt `GBL_INFO`/`VERBOSE` in `ScmHook.c`

**Files:**
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/ScmHook.c`

- [ ] **Step 1: Survey + classify**

```bash
grep -n 'DEBUG ((' GblChainloadPkg/Library/ProtocolHookLib/ScmHook.c
```

Classification rules:
- `scm-sip | smcid=...`, `scm-send | cmd=...`, `mink | obj=...` summary lines: GBL_INFO.
- Anything with raw `s32=`/`r32=` hex bodies (per-call data dumps): VERBOSE.

- [ ] **Step 2: Add include**

After existing `#include <Library/DebugLib.h>`:

```c
#include <Library/GblLog.h>
```

- [ ] **Step 3: Apply renames**

Per the classification above.

- [ ] **Step 4: Build smoke**

```bash
bash scripts/build.sh --mode 1 --auto --debug --verbose 2>&1 | tail -3
```

- [ ] **Step 5: Commit**

```bash
git add GblChainloadPkg/Library/ProtocolHookLib/ScmHook.c
git commit -m "ScmHook: GBL_INFO summary, VERBOSE payload hex

scm-sip / scm-send / mink summary lines use GBL_INFO. Raw s32=/r32=
hex emits use VERBOSE.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 7: Adopt `GBL_INFO`/`VERBOSE` in `VerifiedBootHook.c`

**Files:**
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/VerifiedBootHook.c`

- [ ] **Step 1: Survey + classify**

```bash
grep -n 'DEBUG ((' GblChainloadPkg/Library/ProtocolHookLib/VerifiedBootHook.c
```

Classification:
- `vb-rwstate | op=...`, `vb-fakelock | ...`, `vb-send-rot | ...` summary: GBL_INFO.
- Anything with `bufLen=N | first16=...hex...` payload: VERBOSE.

- [ ] **Step 2: Add include + apply renames + Step 3: Build smoke**

(Same shape as Task 6.)

- [ ] **Step 4: Commit**

```bash
git add GblChainloadPkg/Library/ProtocolHookLib/VerifiedBootHook.c
git commit -m "VerifiedBootHook: GBL_INFO summary, VERBOSE payload hex

vb-* summary lines (rwstate, fakelock, send-rot, milestone, etc.)
use GBL_INFO. Per-call first-16-bytes payload emits use VERBOSE.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 8: Adopt `GBL_INFO`/`VERBOSE` in `SpssHook.c`

**Files:**
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/SpssHook.c`

- [ ] **Step 1: Survey**

```bash
grep -n 'DEBUG ((' GblChainloadPkg/Library/ProtocolHookLib/SpssHook.c
```

SpssHook emits per-call traces of RoT / BootState / Vbh fields. Summary lines (cmd=X, size=Y, st=%r) → GBL_INFO. Hex digest dumps (`digest=...hex...`, `pubKey=...hex...`) → VERBOSE.

- [ ] **Step 2-4: Same shape as previous hook tasks**

- [ ] **Step 5: Commit**

```bash
git add GblChainloadPkg/Library/ProtocolHookLib/SpssHook.c
git commit -m "SpssHook: GBL_INFO summary, VERBOSE digest hex

spss-rot / spss-bootstate / spss-vbh / spss-share summary lines
use GBL_INFO. Per-call digest/pubKey hex emits use VERBOSE.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 9: Adopt `GBL_INFO`/`VERBOSE` in `Mode1Overlay.c` and `UniversalBaseline.c`

**Files:**
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/Mode1Overlay.c`
- Modify: `GblChainloadPkg/Library/ProtocolHookLib/UniversalBaseline.c`

Both files contain one-shot "installed" emits (e.g., "ScmHook: installed 5 of 5 slots") rather than per-call traces. Those become GBL_INFO. If either file has high-volume traces, they become VERBOSE — but likely not on main.

- [ ] **Step 1: Survey both files**

```bash
grep -n 'DEBUG ((' GblChainloadPkg/Library/ProtocolHookLib/Mode1Overlay.c GblChainloadPkg/Library/ProtocolHookLib/UniversalBaseline.c
```

- [ ] **Step 2: Add includes + apply renames in both files**

For each file: add `#include <Library/GblLog.h>` after existing `#include <Library/DebugLib.h>`. Convert DEBUG_INFO emits to GBL_INFO; if any per-call/per-loop emits exist, those become VERBOSE.

- [ ] **Step 3: Build smoke**

```bash
bash scripts/build.sh --mode 1 --auto --debug --verbose 2>&1 | tail -3
```

- [ ] **Step 4: Commit**

```bash
git add GblChainloadPkg/Library/ProtocolHookLib/Mode1Overlay.c GblChainloadPkg/Library/ProtocolHookLib/UniversalBaseline.c
git commit -m "ProtocolHookLib(Mode1Overlay+UniversalBaseline): adopt GBL_INFO

One-shot 'installed N of M slots' summary emits move to GBL_INFO.
No per-call traces here so no VERBOSE needed.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 10: Rewrite `tests/052` as `tests/052_log_minimal.sh`

**Files:**
- Delete: `tests/052_log_stream_split.sh` (if present on main — verify with `ls tests/052*`)
- Create: `tests/052_log_minimal.sh`

The previous test was tightly coupled to PR #17's DebugSink/screen-mask/GBL_DBG_LOGFS_ONLY design. Replace with a regression check for the minimal design.

- [ ] **Step 1: Check whether the old test exists on main**

```bash
ls tests/052* 2>/dev/null
```

If `tests/052_log_stream_split.sh` exists: `git rm tests/052_log_stream_split.sh` in step 4 after writing the new file. If only `tests/052_log_minimal.sh` should exist after this task, that's the goal.

- [ ] **Step 2: Write the new test**

Create `tests/052_log_minimal.sh` with content:

```bash
#!/usr/bin/env bash
# 052_log_minimal.sh — regression check for the minimal logging design.
#
# Asserts:
# 1. GblLog.h exists at the canonical path.
# 2. GBL_INFO and VERBOSE macros are defined and gated on GBL_DEBUG /
#    GBL_VERBOSE respectively.
# 3. The macro bodies use AsciiPrint (visible path) when flagged and
#    DEBUG((DEBUG_INFO, ...)) or no-op (silent / strip) when not.
# 4. No GBL_DBG_LOGFS_ONLY references exist anywhere in the package
#    (legacy from PR #17 design — should not have been merged).
# 5. No GblDebugLib source remains (legacy from PR #17 — should not
#    have been merged).
# 6. BootFlow.c calls LogFsClose() before gBS->LoadImage(...), per the
#    partition-handle release invariant from cleanup-phase-1.
# 7. LogFsLib.h still exports LogFsInit / LogFsWrite / LogFsFlush /
#    LogFsClose / LogFsIsReady (the core API the minimal design relies
#    on).
#
# Host-side check only; on-device runtime verification is manual per
# CLAUDE.md.

set -euo pipefail
cd "$(dirname "$0")/.."

fail=0

# ── Check 1: GblLog.h present ──────────────────────────────────────────────
HDR=GblChainloadPkg/Include/Library/GblLog.h
if [ ! -f "$HDR" ]; then
  echo "FAIL: $HDR not found" >&2
  fail=1
fi

# ── Check 2: GBL_INFO and VERBOSE defined with compile-time gates ──────────
if [ -f "$HDR" ]; then
  if ! grep -qE '^[[:space:]]*#[[:space:]]*if[[:space:]]*\(GBL_DEBUG' "$HDR"; then
    echo "FAIL: GblLog.h does not have a #if (GBL_DEBUG ...) gate" >&2
    fail=1
  fi
  if ! grep -qE '^[[:space:]]*#[[:space:]]*if[[:space:]]*\(GBL_VERBOSE' "$HDR"; then
    echo "FAIL: GblLog.h does not have a #if (GBL_VERBOSE ...) gate" >&2
    fail=1
  fi
  if ! grep -qE 'define[[:space:]]+GBL_INFO' "$HDR"; then
    echo "FAIL: GBL_INFO macro not defined in GblLog.h" >&2
    fail=1
  fi
  if ! grep -qE 'define[[:space:]]+VERBOSE' "$HDR"; then
    echo "FAIL: VERBOSE macro not defined in GblLog.h" >&2
    fail=1
  fi
fi

# ── Check 3: GBL_INFO uses AsciiPrint and DEBUG((DEBUG_INFO,...)) ──────────
if [ -f "$HDR" ]; then
  if ! grep -q 'AsciiPrint' "$HDR"; then
    echo "FAIL: GblLog.h does not reference AsciiPrint" >&2
    fail=1
  fi
  if ! grep -qE 'DEBUG[[:space:]]*\(\([[:space:]]*DEBUG_INFO' "$HDR"; then
    echo "FAIL: GblLog.h does not reference DEBUG((DEBUG_INFO, ...))" >&2
    fail=1
  fi
fi

# ── Check 4: GBL_DBG_LOGFS_ONLY fully absent ───────────────────────────────
if grep -rn 'GBL_DBG_LOGFS_ONLY' GblChainloadPkg 2>/dev/null | grep -v 'Binary file'; then
  echo "FAIL: GBL_DBG_LOGFS_ONLY references found (PR #17 legacy that shouldn't be present)" >&2
  fail=1
fi

# ── Check 5: GblDebugLib source absent ─────────────────────────────────────
if [ -d GblChainloadPkg/Library/GblDebugLib ]; then
  echo "FAIL: GblChainloadPkg/Library/GblDebugLib/ exists — should not be present" >&2
  fail=1
fi

# ── Check 6: BootFlow.c calls LogFsClose before LoadImage ──────────────────
BF=GblChainloadPkg/Application/GblChainload/BootFlow.c
if [ -f "$BF" ]; then
  if ! awk '/gBS->LoadImage/{exit} {print}' "$BF" | grep -q 'LogFsClose'; then
    echo "FAIL: BootFlow.c must call LogFsClose() before gBS->LoadImage" >&2
    fail=1
  fi
fi

# ── Check 7: LogFsLib.h public API intact ──────────────────────────────────
LF=GblChainloadPkg/Include/Library/LogFsLib.h
for sym in LogFsInit LogFsWrite LogFsFlush LogFsClose LogFsIsReady; do
  if ! grep -q "$sym" "$LF"; then
    echo "FAIL: LogFsLib.h missing $sym" >&2
    fail=1
  fi
done

if [ "$fail" -ne 0 ]; then
  exit 1
fi
echo "OK: minimal logging design constants and structure in place."
```

- [ ] **Step 3: Make executable**

```bash
chmod +x tests/052_log_minimal.sh
```

- [ ] **Step 4: Delete the old test if present**

```bash
if [ -f tests/052_log_stream_split.sh ]; then
  git rm tests/052_log_stream_split.sh
fi
```

- [ ] **Step 5: Run the new test**

```bash
bash tests/052_log_minimal.sh
```

Expected: `OK: minimal logging design constants and structure in place.`

- [ ] **Step 6: Commit**

```bash
git add tests/052_log_minimal.sh
git commit -m "tests/052: replace with log-minimal regression check

Drops the PR #17 design's checks (DebugSink screen mask,
GBL_DBG_LOGFS_ONLY, GblDebugLib presence). Adds checks for the
minimal design: GblLog.h presence, macro structure with the right
gates, LogFsClose-before-LoadImage discipline, LogFsLib.h API
intact, no PR #17 legacy tokens.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 11: Extend `tests/010` with strip verification

**Files:**
- Modify: `tests/010_build_smoke.sh`

Strip-verification confirms the VERBOSE compile-strip is actually doing what the macro promises.

- [ ] **Step 1: Locate the end of the existing `010` script**

```bash
tail -10 tests/010_build_smoke.sh
```

Identify where the existing assertions end. The new block goes after the last existing build/check.

- [ ] **Step 2: Append the strip-verification block**

Add the following at the end of `tests/010_build_smoke.sh`:

```bash

# ── Strip-verification for VERBOSE compile-strip ─────────────────────────
# VERBOSE() format strings must be ABSENT from non-verbose builds (compile-strip
# zeroes the macro body, so no .rodata entry is emitted). They must be PRESENT
# in verbose builds.

# We probe by looking for a format string fragment unique to a VERBOSE() call
# site. AblUnwrap's per-section trace ("section @ 0x") is a good marker —
# emitted only from VERBOSE() (per the minimal design's Task 4 retag).
PROBE_FRAG='section @ 0x'

for v in dist/mode-1-auto.efi dist/mode-1-auto-debug.efi; do
  [ -f "$v" ] || continue
  if strings "$v" 2>/dev/null | grep -q "$PROBE_FRAG"; then
    echo "FAIL: VERBOSE format string '$PROBE_FRAG' present in non-verbose build $v" >&2
    fail=1
  fi
done

if [ -f dist/mode-1-auto-debug-verbose.efi ]; then
  if ! strings dist/mode-1-auto-debug-verbose.efi 2>/dev/null | grep -q "$PROBE_FRAG"; then
    echo "WARN: VERBOSE format string '$PROBE_FRAG' not visible via strings in verbose build" >&2
    echo "      Compiler may have deduped/stripped string literals; manual nm/objdump check needed." >&2
  fi
fi
```

If `tests/010_build_smoke.sh` doesn't currently define `fail`, add a `fail=0` initialization at the script's top before this block runs.

- [ ] **Step 3: Run**

```bash
bash tests/010_build_smoke.sh 2>&1 | tail -10
```

Expected: previous assertions still pass; new strip-verification reports OK for non-verbose builds and WARN-at-worst for the verbose build (the WARN is acceptable per Task 4's strip-verification — string literals may not survive the GenFw step).

- [ ] **Step 4: Commit**

```bash
git add tests/010_build_smoke.sh
git commit -m "tests/010: VERBOSE() compile-strip verification

After all four build variants are produced, scan the non-verbose
binaries for the AblUnwrap scan-loop format string ('section @ 0x')
which is only emitted from VERBOSE(). It must be absent in
mode-1-auto.efi and mode-1-auto-debug.efi; present in
mode-1-auto-debug-verbose.efi (best-effort — string literals may
not survive GenFw symbol-stripping).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 12: Run the full test suite

**Files:** none modified.

- [ ] **Step 1: Run all tests**

```bash
bash tests/runall.sh 2>&1 | tail -20
```

Expected: `ALL TESTS PASS` (or equivalent). If any test fails, address it before proceeding.

- [ ] **Step 2: Commit (only if any fixup edits were needed in Step 1)**

If Step 1 needed any fixes, commit them with a descriptive message. Otherwise skip.

---

## Task 13: On-device verification (manual — user runs)

**Files:** none modified.

Subagents do NOT run fastboot per CLAUDE.md. This task is a procedure the user executes; the agent verifies the result by reading the pulled logs.

- [ ] **Step 1: Stage and run the prod variant**

```
fastboot stage dist/mode-1-auto.efi
fastboot oem boot-efi
# (escape to fastboot via timeout; reboot to recovery; pull logs)
```

- [ ] **Step 2: Verify prod behavior**

```bash
LATEST=$(ls -td logs/*/ | head -1)
echo "Inspecting: $LATEST"

# UefiLog should have our boundary markers (Print() calls — always visible).
grep -c 'gbl-chainload entered\|BootFlow: start' "$LATEST/logfs/"UefiLog*.txt 2>/dev/null
# Our internal log should also have them (DebugSink mirror).
grep -c 'gbl-chainload entered\|BootFlow: start' "$LATEST/logfs/gbl-chainload_Boot"*.txt 2>/dev/null

# VERBOSE content (e.g., AblUnwrap section traces) should be ABSENT.
grep -c 'section @ 0x' "$LATEST/logfs/"UefiLog*.txt 2>/dev/null
```

Expected: boundary markers > 0 in both locations; `section @ 0x` count == 0.

- [ ] **Step 3: Stage and run the --debug variant**

```
fastboot stage dist/mode-1-auto-debug.efi
fastboot oem boot-efi
# ... reboot to recovery, pull logs
```

- [ ] **Step 4: Verify --debug behavior**

```bash
LATEST=$(ls -td logs/*/ | head -1)
grep -c 'BootFlow: patches applied' "$LATEST/logfs/"UefiLog*.txt 2>/dev/null   # > 0
grep -c 'section @ 0x' "$LATEST/logfs/"UefiLog*.txt 2>/dev/null                 # == 0
```

Expected: patches-applied count > 0 (GBL_INFO emits via AsciiPrint → screen + UefiLog); VERBOSE content still 0.

- [ ] **Step 5: Stage and run the --debug --verbose variant**

```
fastboot stage dist/mode-1-auto-debug-verbose.efi
fastboot oem boot-efi
# ... reboot, pull
```

- [ ] **Step 6: Verify verbose behavior**

```bash
LATEST=$(ls -td logs/*/ | head -1)
grep -c 'section @ 0x' "$LATEST/logfs/"UefiLog*.txt 2>/dev/null   # > 0
grep -c 'qsee-buf\|scm-send-buf\|vb-rwstate-buf\|spss-rot' "$LATEST/logfs/"UefiLog*.txt 2>/dev/null   # > 0 (hook hex dumps)
```

Expected: both > 0 — VERBOSE content visible in UefiLog under --verbose builds.

- [ ] **Step 7: Confirm no boot regressions across all three variants**

For each of the three variants, the device should successfully chain-load the patched ABL and reach recovery without a hard power-off. Document any anomalies.

- [ ] **Step 8: Commit a verification record**

Append a section to the spec recording the verification outcome:

```bash
git checkout feature/logging-minimal-impl
cat >> docs/superpowers/specs/2026-05-13-logging-minimal-design.md <<EOF

---

## 10. On-device verification

Verified on-device $(date +%Y-%m-%d) against canoe (OnePlus 12R, SM8550):
- **prod** (\`mode-1-auto.efi\`): boundary markers in UefiLog and gbl-chainload_BootN.txt; VERBOSE content absent.
- **--debug** (\`mode-1-auto-debug.efi\`): GBL_INFO emits visible in UefiLog via AsciiPrint; VERBOSE absent.
- **--debug --verbose** (\`mode-1-auto-debug-verbose.efi\`): VERBOSE content (\`section @ 0x\`, \`qsee-buf\`, etc.) present in UefiLog.
- No boot regressions across any variant.
EOF
git add docs/superpowers/specs/2026-05-13-logging-minimal-design.md
git commit -m "spec: log-minimal verified on-device"
```

(Adjust the recorded date and any per-variant nuances to match actual results.)

---

## Self-Review

**Spec coverage check (each section of the spec → which task implements it):**

| Spec section | Implemented by |
|--------------|----------------|
| §1 goals | Tasks 1, 2-9 (collectively achieve the macro-based logging) |
| §2 investigation findings | No code task — informational context |
| §3 tier table | Task 1 (GblLog.h macros) |
| §4.1 GblLog.h | Task 1 |
| §4.2 LogFsLib reduced | Task 0 step 4 verifies main's DebugSink is already simple. No reduction needed beyond what main has. |
| §4.3 dual emit pattern | Tasks 2-9 follow the pattern at call sites |
| §4.4 LogFsClose discipline | Task 0 step 4 + Task 10 check 6 verify |
| §4.5 hooks use AsciiPrint via GBL_INFO/VERBOSE | Tasks 5-9 |
| §4.6 DSC changes | Task 0 step 2 verifies main already has them; no DSC edit needed |
| §4.7 build flags | Task 0 step 5 verifies main already has them |
| §5.1 tests/052 rewrite | Task 10 |
| §5.2 tests/010 strip-verify | Task 11 |
| §5.3 on-device | Task 13 |
| §6 rollout | This plan IS the option-B rollout (fresh branch off main); spec recommends closing #17 separately (user decision, not in plan) |
| §7 explicit drops list | All items are PR #17 work that isn't on main — Task 0 verifies absence |
| §8 trade-offs | Documented; no code |
| §9 out of scope | Documented; no code |

**Placeholder scan:** the only "TBD-ish" element is the speculative classification in Tasks 5-9 ("if main has these specific lines"). That's necessary because the agent must read the actual file content; can't pre-write the rename diff without seeing it. Classification rules are concrete.

**Type consistency:** macro names `GBL_INFO`, `VERBOSE` used consistently across Tasks 1-9 and tests/052. Header path `GblChainloadPkg/Include/Library/GblLog.h` consistent. Format string convention (ASCII / CHAR8) consistent.

**Backwards-compat:** main has `LogFsLib/DebugSink.c` which auto-mirrors ConOut→logfs. Under our new design, `AsciiPrint()` (under --debug) goes through ConOut→DebugSink→logfs too. So the existing DebugSink continues to mirror our emits. No regression.

---

## Plan complete

Saved to `docs/superpowers/plans/2026-05-13-logging-minimal.md`. Two execution options:

1. **Subagent-Driven** (recommended) — fresh subagent per task; two-stage review between tasks. Best fit for the bulk-rename tasks (5-9) which all follow the same pattern but need actual-file inspection.

2. **Inline Execution** — execute tasks in this session via executing-plans, batch with checkpoints. Faster, no per-task agent invocation overhead.

Which approach?
