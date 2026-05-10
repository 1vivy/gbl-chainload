# Logfs Verbosity & Failed-Boot Capture Audit — Track 1 Design

**Status:** approved — ready for implementation plan.

**Depends on:** Track 0 (`2026-05-10-test-device-hardening-design.md`)
must be merged first so verification of this work is reliable.

## Goal

`GblChainload_BootN.txt` reliably contains the verbose triage data we'd
want next time a boot fails, without flooding the small logfs partition.

## Why this is needed

The most recent system-context capture showed
`GblChainload_Boot{1,2}.txt` at exactly **224 bytes** — banner-only, no
body. That's a smoking gun: either `LogFsInstallDebugSink` isn't
installing before output starts, output isn't routing through
`gST->ConOut->OutputString`, or the captured slot was a boot path that
didn't reach our verbose code.

In contrast an earlier (recovery-context) Boot1.txt had ~20 lines and
~600 bytes — so capture *can* work; something is path-dependent.

`PcdDebugPrintErrorLevel` is currently `0x80000042` (ERROR + WARN +
INFO). `DEBUG_VERBOSE` (`0x00400000`) is filtered out before reaching
`OutputString`, so any `DEBUG((DEBUG_VERBOSE, ...))` in our hooks
silently drops.

## Architecture

Three sub-tasks executed in order.

### Sub-task 1: audit & repro

Capture from a fresh cold boot via the (Track-0-hardened)
`scripts/test-device.sh`. Inspect every `GblChainload_BootN.txt` slot.
Cross-reference against the BootFlow decision tree to identify where
output goes missing.

Specific questions to answer:

- Does `LogFsInstallDebugSink` install on every boot path that opens a
  log slot, or are some paths writing only the banner?
- Are there `Print(...)` / `DEBUG(...)` calls happening *before*
  `LogFsInstallDebugSink` runs? Those go to console only and miss the
  file.
- Does any path call `LogFsRemoveDebugSink` early (e.g. before all the
  interesting output is emitted)?
- Are some paths emitting via a serial-only mechanism that bypasses
  `gST->ConOut`?

Output: a one-page findings doc in `docs/re/logfs-capture-gaps.md`
listing each gap with file:line citations. The findings shape sub-task 3.

### Sub-task 2: PCD widen

`GblChainloadPkg/GblChainloadPkg.dsc`:

```
gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000042
```
becomes
```
gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80400042
```

This adds `DEBUG_VERBOSE` (bit 0x00400000) to the mask. Existing
`DEBUG((DEBUG_VERBOSE, ...))` calls in ScmHook, QseecomHook,
VerifiedBootHook (and anywhere else) start landing in the file. Zero
source changes; rebuild only.

We deliberately do **not** widen to `0xFFFFFFFF` — pulling in
`DEBUG_LOAD`, `DEBUG_BLKIO`, `DEBUG_POOL`, `DEBUG_PAGE`, etc. floods
the log with framework-internal events we don't own and can't act on.

### Sub-task 3: targeted instrumentation pass

Add `DEBUG((DEBUG_INFO, ...))` (always passes the PCD filter) at
decision points the audit identifies as gaps. Targets, in priority
order:

1. **BootFlow mode-selection.** When BootFlow chooses mode-0 vs mode-1
   vs mode-fakelocked vs FastbootLib, log the chosen branch and the
   reason (key window, mode token, default).
2. **Per-patch attempt result.** DynamicPatchLib already logs
   "patch9-avb-locked-recoverable-continue [mode-1, mandatory] -> OK"
   — verify this is reliable across all paths.
3. **Swallow vs passthrough decision.** Each hooked SCM SIP /
   Qseecom / SPSS call: log whether we let it through, swallowed it,
   or transformed the result. This data is essential for Track 3
   (oplus fastboot-sec investigation).
4. **VerifiedBoot inputs/outputs.** ROT bytes, vbmeta hash, computed
   verifiedboot state — log on entry and exit of the hook.

Implementation discipline: each new log line should be a one-liner
with a clear prefix (e.g. `BootFlow:`, `ScmHook:`, `Vb:`) so grep
remains useful.

## Validation flow

- Branch off `main`.
- Apply sub-task 2 + 3 together (or land sub-task 1's findings doc
  first if the work splits naturally).
- `./scripts/test-device.sh` from a clean cold boot.
- Diff log size and content against the current ~600 B baseline.
- Acceptance criteria:
  - per-boot `GblChainload_BootN.txt` < 50 KB
  - log content includes verbose lines that aid triage
  - each known mode (0, 1, fakelocked, FastbootLib) produces
    distinguishable output
- If acceptance criteria pass: merge to main.
- If too noisy: trim instrumentation (keep PCD) or trim PCD too. Don't
  iterate forever — if we can't fit in the budget after one trim, drop
  back to PCD-only and ship that.

## Out of scope

- Rotation depth changes (5 slots stays)
- "Pin" command for archiving a known-bad boot
- Pre-GBL log changes (UefiLog rotation already works)
- `0xFFFFFFFF` PCD
- Restructuring DebugSink mechanics

## Files in scope

- `GblChainloadPkg/GblChainloadPkg.dsc` (PCD)
- `GblChainloadPkg/Application/GblChainload/BootFlow.c` (mode decision logging)
- `GblChainloadPkg/Library/ProtocolHookLib/ScmHook.c`
- `GblChainloadPkg/Library/ProtocolHookLib/QseecomHook.c`
- `GblChainloadPkg/Library/ProtocolHookLib/SpssHook.c`
- `GblChainloadPkg/Library/ProtocolHookLib/VerifiedBootHook.c`
- `GblChainloadPkg/Library/DynamicPatchLib/oem/oneplus_canoe.c` (audit only)
- `GblChainloadPkg/Library/LogFsLib/DebugSink.c` (only if sub-task 1 reveals a wiring bug)
- New: `docs/re/logfs-capture-gaps.md` (findings)

## Risks and known gotchas

- Sub-task 1 might surface that the 224-byte anomaly is a real DebugSink
  wiring bug, not a verbosity issue. In that case the fix is "install
  sink earlier" or "explicitly flush before mode branch", and the
  PCD/instrumentation work is a separate, smaller follow-up.
- Logfs partition is small (FAT, a few MB). 5 slots × verbose output
  must fit. Sub-task 3 must measure per-boot size and stay under
  budget. If it doesn't, prefer dropping instrumentation over
  changing rotation depth.
- The 5-slot ring means if you don't pull within 5 boots of a failed
  boot, the relevant log gets clobbered. This is a known trade-off,
  not in scope for this plan.
