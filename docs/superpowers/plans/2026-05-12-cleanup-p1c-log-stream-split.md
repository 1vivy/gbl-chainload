# Cleanup Phase 1 — PR-c: Log Stream Split Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop our verbose output from being mirrored into `UefiLogN.txt` (which is owned by QCOM BDS and carries the PBL→XBL→ABL→BDS prelude). Verbose output goes **only** to the dedicated gbl-chainload stream, which is also renamed from `GblChainload_BootN.txt` to `gbl-chainload_BootN.txt` to match repo casing convention. UefiLog retains only `EFI_D_ERROR`-class lines plus two boundary markers ("gbl-chainload entered" / "gbl-chainload exiting (rc=...)").

**Architecture:** Single-file routing change in `GblChainloadPkg/Library/LogFsLib/DebugSink.c`. The two-stream design is already in place (`Mount.c` already opens both handles; per `memory/active_investigation_log_flush.md` the per-write flush contract is intact). Two concrete changes: (1) flip the verbose level mask so only ERROR plus boundary tags hit the UefiLog handle, and (2) rename the gbl-chainload file constant. No new modules. No new tests beyond a host-side grep regression that asserts the file-name constant is the new lowercase one.

**Tech Stack:** EDK2 (C), LogFsLib internals.

**Spec reference:** `docs/superpowers/specs/2026-05-12-cleanup-phase-1-design.md`, section "PR-c: Logging stream split (P3)".

**Pre-existing context (do not violate):**
- The per-write flush in `LogFsWrite` (from `memory/active_investigation_log_flush.md`) is **non-negotiable**. Do not remove or batch-defer it. If you touch `Mount.c`, preserve the flush call after every successful write.
- Boot-index rotation lives in `Rotation.c`. Re-use it; do not write a parallel index scheme.

---

## File Structure

- Modify: `GblChainloadPkg/Library/LogFsLib/DebugSink.c` (re-route per-level routing — main change).
- Modify: `GblChainloadPkg/Library/LogFsLib/Mount.c` (filename constant only; preserve all flush behavior).
- Modify (optional, only if file-name constant lives there): `GblChainloadPkg/Library/LogFsLib/LogFsLib.h` or equivalent.
- Create: `tests/051_log_stream_split.sh` (regression check on file-name constant + presence of boundary markers in source).

---

## Task 1: Create feature branch and inventory LogFsLib

**Files:** none.

- [ ] **Step 1: Branch off main**

Run:
```bash
git checkout main
git checkout -b feature/cleanup-p1c-log-stream-split
git status -s
```

Expected: switched to new branch; status shows only the unrelated ` M edk2` submodule (if any) and is otherwise clean.

- [ ] **Step 2: Inventory LogFsLib sources**

Run: `ls GblChainloadPkg/Library/LogFsLib/`

Expected files (at least): `Mount.c`, `DebugSink.c`, `Rotation.c`, plus an INF file and possibly headers. Note any unexpected file — it may carry the routing logic.

- [ ] **Step 3: Locate the gbl-chainload filename constant**

Run:
```bash
rg -n 'GblChainload_Boot|gbl-chainload_Boot|"GblChainload"' GblChainloadPkg/Library/LogFsLib/
```

Expected: one or two hits, typically a `#define` or a `CONST CHAR16` literal in `Mount.c` or a shared header. Record file + line.

- [ ] **Step 4: Locate the UefiLog mirror sink**

Run:
```bash
rg -n 'UefiLog|UEFI_LOG|gUefiLog|UefiLogHandle' GblChainloadPkg/Library/LogFsLib/
```

Expected: hits in `DebugSink.c` (and possibly `Mount.c`). The pattern is typically a write call inside the DEBUG-sink callback that writes to *both* handles. That's the line(s) to gate.

- [ ] **Step 5: Identify boundary-marker emit sites**

Run:
```bash
rg -n 'gbl-chainload entered|gbl-chainload exiting|GBL_BUILD_NAME.*entered|GBL_BUILD_NAME.*exiting' GblChainloadPkg/ edk2/QcomModulePkg/Library/FastbootLib/
```

Expected: at least two hits — one at module entry (`UefiMain` or similar in `GblChainloadPkg/Application/`) and one before chainload handoff or fastboot-fallback drop. Note both call sites; Task 5 routes these explicitly to the UefiLog handle.

---

## Task 2: Inspect captured logs to confirm current behavior

**Files:** read-only — `logs/<recent>/logfs/`.

- [ ] **Step 1: Pick a recent capture with debug+verbose**

Run:
```bash
ls -td logs/2026* | head -1
LATEST=$(ls -td logs/2026* | head -1)
ls "$LATEST/logfs/" 2>/dev/null
```

Expected: at least one entry like `logs/20260510-011140_manual_manual_v2.0-plan1`. Its `logfs/` should contain both `UefiLog*` files and `GblChainload_Boot*` files.

- [ ] **Step 2: Confirm the double-write problem**

Pick one boot index, e.g. `1`. Run:
```bash
grep -c 'DynamicPatch:' "$LATEST/logfs/UefiLog1.txt" 2>/dev/null || true
grep -c 'DynamicPatch:' "$LATEST/logfs/UefiLogSaved1.txt" 2>/dev/null || true
grep -c 'DynamicPatch:' "$LATEST/logfs/GblChainload_Boot1.txt" 2>/dev/null || true
```

Expected: non-zero hits in both `UefiLog*1.txt` and `GblChainload_Boot1.txt` — confirming verbose lines currently land in both streams. If `UefiLog*1.txt` has zero `DynamicPatch:` lines, the issue is already fixed and this PR is a no-op for routing (only the filename rename remains). Validate with the user before continuing.

- [ ] **Step 3: Confirm the BDS prelude is being truncated**

Run:
```bash
head -20 "$LATEST/logfs/UefiLog1.txt"
```

Expected: ideally PBL/XBL/ABL banners or QCOM BDS strings. If the file starts with our `DynamicPatch:` or `LogFs:` output, the BDS prelude has been flushed out by our writes — exactly the problem this PR fixes.

---

## Task 3: Write the regression test

**Files:** Create: `tests/051_log_stream_split.sh`.

- [ ] **Step 1: Author the test**

Create `tests/051_log_stream_split.sh`:

```bash
#!/usr/bin/env bash
# 051_log_stream_split.sh — regression check for cleanup-phase-1 PR-c.
#
# Asserts:
# 1. The gbl-chainload log filename constant in LogFsLib is the new
#    lowercase-hyphen form: gbl-chainload_Boot
# 2. The old form (GblChainload_Boot) is not referenced as an output
#    file path anywhere in LogFsLib source.
# 3. DebugSink writes to UefiLog only for ERROR-level OR boundary-marker
#    paths — checked by absence of unconditional mirror writes inside
#    a code path keyed on EFI_D_INFO/EFI_D_VERBOSE.
#
# Host-side check; runtime stream split is verified manually on-device
# (see plan Task 8).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOGFS="$ROOT/GblChainloadPkg/Library/LogFsLib"

fail=0

# Check 1: new filename constant present.
if ! rg -q 'gbl-chainload_Boot' "$LOGFS"; then
  echo "FAIL: 'gbl-chainload_Boot' filename constant not found in $LOGFS" >&2
  fail=1
fi

# Check 2: old filename constant absent.
if rg -n 'GblChainload_Boot' "$LOGFS"; then
  echo "FAIL: old 'GblChainload_Boot' name still present (above)" >&2
  fail=1
fi

# Check 3: no unconditional UefiLog writes inside an INFO/VERBOSE branch.
# Heuristic: look for back-to-back calls writing the same buffer to
# both gGblBootLogHandle (or equivalent) and the UefiLog handle without
# a level guard between them. Conservative regex; intent is to catch
# obvious mirror-writes, not to be exhaustive.
if rg -nP '(?s)gGblBootLogHandle.*Write[^}]{0,200}UefiLog[A-Za-z]*Write' \
       "$LOGFS/DebugSink.c" >/dev/null 2>&1; then
  echo "WARN: possible unguarded mirror-write in DebugSink.c — manually verify" >&2
fi

if [ "$fail" -ne 0 ]; then
  exit 1
fi
echo "OK: log stream split constants in place."
```

- [ ] **Step 2: Make it executable**

Run: `chmod +x tests/051_log_stream_split.sh`

- [ ] **Step 3: Run — expect FAIL**

Run: `tests/051_log_stream_split.sh`

Expected: exit 1, with `FAIL: 'gbl-chainload_Boot' filename constant not found ...`. This proves the test catches the pre-change state.

---

## Task 4: Rename the gbl-chainload file constant

**Files:** Modify whichever LogFsLib file holds the constant (per Task 1 Step 3).

- [ ] **Step 1: Open the file with the constant**

From Task 1 Step 3 you have the file:line. Open it.

- [ ] **Step 2: Change the constant**

Replace every occurrence of `GblChainload_Boot` with `gbl-chainload_Boot` **within LogFsLib only**. Specifically: the UEFI file-system creates files via wide-char paths, so the constant is likely a `CONST CHAR16 *` or an `L"..."`-literal `#define`. Update:

```c
// before
#define GBL_BOOT_LOG_PREFIX  L"GblChainload_Boot"
// after
#define GBL_BOOT_LOG_PREFIX  L"gbl-chainload_Boot"
```

(Use the actual macro/variable name from the file. If it's a direct literal in `Mount.c` rather than a macro, edit the literal in place.)

- [ ] **Step 3: Verify the symbol still resolves**

Run:
```bash
rg -n 'GBL_BOOT_LOG_PREFIX|gbl-chainload_Boot|GblChainload_Boot' GblChainloadPkg/Library/LogFsLib/
```

Expected: every reference to the symbol resolves to the new lowercase literal. The old `GblChainload_Boot` literal must not appear anywhere in the LogFsLib source tree.

- [ ] **Step 4: Confirm there are no consumers outside LogFsLib**

Run:
```bash
rg -n 'GblChainload_Boot' GblChainloadPkg/ scripts/ tests/
```

Expected: zero hits (the regression test in Task 3 references the *string* via `rg`, so its self-check is fine; it scans `LogFsLib/` only).

---

## Task 5: Route verbose output to the gbl-chainload handle only

**Files:** Modify: `GblChainloadPkg/Library/LogFsLib/DebugSink.c`.

- [ ] **Step 1: Read DebugSink.c top-to-bottom**

Run: `wc -l GblChainloadPkg/Library/LogFsLib/DebugSink.c && head -100 GblChainloadPkg/Library/LogFsLib/DebugSink.c`

You're looking for the EDK2 `DebugPrintMarker` or `DebugPrint`/`DebugSink` callback — the function called by the `DEBUG ((EFI_D_*, ...))` macro. Typical signature:

```c
VOID EFIAPI
LogFsDebugSink (
  IN UINTN         ErrorLevel,
  IN CONST CHAR8  *Format,
  IN VA_LIST       Marker
  );
```

- [ ] **Step 2: Identify the current mirror behavior**

Inside the sink, locate the calls that write to the UefiLog handle and the gbl-chainload handle. The pre-PR-c form is roughly:

```c
// Current (verbose lines go to BOTH streams)
LogFsHandleWrite (gGblBootLogHandle, buf, len);
LogFsHandleWrite (gUefiLogHandle,    buf, len);  // ← the unconditional mirror
```

If the structure is different (e.g. a single dispatch with a level-bitmap), adapt the rest of this task to match.

- [ ] **Step 3: Replace with a level-gated write to the UefiLog handle**

Change the unconditional `LogFsHandleWrite (gUefiLogHandle, buf, len);` to:

```c
if ((ErrorLevel & EFI_D_ERROR) != 0) {
  LogFsHandleWrite (gUefiLogHandle, buf, len);
}
```

Keep the unconditional write to `gGblBootLogHandle` — every DEBUG line lands in our stream regardless of level.

- [ ] **Step 4: Verify the EFI_D_ERROR mask covers fatal lines only**

Run: `rg -n 'EFI_D_ERROR|EFI_D_FATAL' GblChainloadPkg/Library/LogFsLib/`

Expected: confirm `EFI_D_ERROR` is the correct macro (EDK2 standard). If the codebase uses `DEBUG_ERROR` (newer alias), use that name instead — match local convention.

- [ ] **Step 5: Preserve the flush call**

After your edited write block, confirm the existing per-write flush is still invoked. From `memory/active_investigation_log_flush.md`: every successful write to the gbl-chainload handle must be followed by `Flush`. Do NOT route the flush behind the `EFI_D_ERROR` gate.

Run: `rg -n 'Flush' GblChainloadPkg/Library/LogFsLib/DebugSink.c GblChainloadPkg/Library/LogFsLib/Mount.c`

Expected: flush call present and unguarded relative to level.

---

## Task 6: Route boundary markers explicitly to UefiLog

**Files:** Modify call sites identified in Task 1 Step 5 (typically `GblChainloadPkg/Application/<name>.c`).

The two boundary markers — "gbl-chainload entered" at module entry and "gbl-chainload exiting (rc=...)" before chainload handoff or fastboot-fallback drop — must remain visible on UefiLog after Task 5's level-gate. Task 5 gates UefiLog writes on `EFI_D_ERROR`, so the markers need to be emitted at `EFI_D_ERROR` to survive the gate. (`EFI_D_ERROR` in EDK2 is a *bit* in a level mask, not a severity grade — promoting always-on tracers to that bit is the conventional EDK2 pattern.)

- [ ] **Step 1: Inspect the call sites Task 1 Step 5 found**

Branch on what you found:

- **Both markers already emit via `DEBUG (...)` calls** → go to Step 2.
- **Only one of the two markers exists, or neither** → go to Step 3.
- **More than two boundary-marker hits** → STOP and escalate; the call sites need a designed cleanup before this PR.

- [ ] **Step 2 (markers exist): Promote them to `EFI_D_ERROR`**

For each existing marker call, change the level argument to `EFI_D_ERROR`:

```c
// before
DEBUG ((EFI_D_INFO, "gbl-chainload entered (mode=%d)\n", Mode));
// after
DEBUG ((EFI_D_ERROR, "gbl-chainload entered (mode=%d)\n", Mode));
```

```c
// before
DEBUG ((EFI_D_INFO, "gbl-chainload exiting (rc=%r)\n", Status));
// after
DEBUG ((EFI_D_ERROR, "gbl-chainload exiting (rc=%r)\n", Status));
```

Skip to Step 4.

- [ ] **Step 3 (markers missing or partial): Add them**

The spec requires both markers be present on UefiLog. If they're missing, add them.

**Entry marker** — at the top of the application's entry point (typically `UefiMain` in `GblChainloadPkg/Application/<name>.c`), just after argument parsing and before any patch / hook installation:

```c
DEBUG ((EFI_D_ERROR, "gbl-chainload entered (mode=%d, build=%a)\n",
        Mode, GBL_BUILD_NAME));
```

(Use whatever local variable already carries the mode — search `rg -n 'Mode ?=|gMode|CONFIG_MODE' GblChainloadPkg/Application/`. If no mode variable is in scope at the entry point, omit it: `"gbl-chainload entered (build=%a)\n"`.)

**Exit marker** — at the chainload-handoff site (just before `StartImage` or whatever transfers control) AND at the fastboot-fallback drop (just before `FastbootInitialize` or whatever starts the fallback loop). Both sites get the same one-liner:

```c
DEBUG ((EFI_D_ERROR, "gbl-chainload exiting (rc=%r, path=%a)\n",
        Status, kHandoffPath));
```

…where `kHandoffPath` is a per-site string literal like `"chainload"` or `"fastboot-fallback"`. Define them at file scope:

```c
STATIC CONST CHAR8 *kHandoffPath = "chainload";        // at chainload site
STATIC CONST CHAR8 *kHandoffPath = "fastboot-fallback"; // at fallback site
```

(Easier: put the literal directly in each `DEBUG` call rather than via a variable.)

- [ ] **Step 4: Do NOT promote any other INFO line**

Run: `rg -nC1 'DEBUG.*EFI_D_ERROR' GblChainloadPkg/Application/`

Expected: only the two boundary markers (and any pre-existing genuine errors). If you accidentally promoted others (or Step 3 created extras), revert them.

- [ ] **Step 5: Re-verify Task 1 Step 5's grep finds both markers**

Run:
```bash
rg -n 'gbl-chainload entered|gbl-chainload exiting' GblChainloadPkg/
```

Expected: at least two hits — one entered, one exiting. Both at `EFI_D_ERROR` level.

---

## Task 7: Build smoke

**Files:** none.

- [ ] **Step 1: Build mode-1 dev-capture variant**

Run: `./scripts/build.sh --mode 1 --auto --debug --verbose`

Expected: build completes, `dist/mode-1-auto-debug-verbose.efi` updated. If build fails:
- Linker error for `gUefiLogHandle` or `gGblBootLogHandle` — you removed a forward declaration or duplicated a definition. Compare against the pre-edit DebugSink.c.
- "Implicit declaration of `LogFsHandleWrite`" — missing include or the symbol you used doesn't match the actual API. Adjust to the real name (run `rg -n 'LogFsHandle|HandleWrite' GblChainloadPkg/Library/LogFsLib/` for the canonical name).

- [ ] **Step 2: Build mode-0 dev-capture variant**

Run: `./scripts/build.sh --mode 0 --auto --debug --verbose`

Expected: build completes.

- [ ] **Step 3: Build the production (non-verbose) variants**

Run:
```bash
./scripts/build.sh --mode 0
./scripts/build.sh --mode 1
```

Expected: both succeed. Sizes for the production variants should be within ~2 KiB of pre-PR-c values (the filename rename adds 1 byte and the new conditional adds ~4 bytes; expect a flat or marginally smaller binary).

---

## Task 8: Run regression test — expect PASS

**Files:** none.

- [ ] **Step 1: Re-run the test from Task 3**

Run: `tests/051_log_stream_split.sh`

Expected: exit 0, output `OK: log stream split constants in place.`

If FAIL on "old name still present": you missed a reference in LogFsLib. Run the rg from Task 4 Step 3 again, fix, repeat.

---

## Task 9: On-device smoke verification

**Files:** none (captures land in `logs/<timestamp>`).

- [ ] **Step 1 (manual, on-device): boot the dev-capture build**

This step requires the test device in fastboot. From your shell:

```bash
fastboot stage dist/mode-1-auto-debug-verbose.efi
fastboot oem boot-efi
```

Expected: device boots Android. Verbose output should be visible on the on-screen log (if `--auto` is in effect) but UefiLog should NOT be flooded.

- [ ] **Step 2: Pull the log capture**

Use the existing capture mechanism. Likely:

```bash
./scripts/device-monitor.sh --capture-only
# or
./scripts/test-device-manual.sh --pull-logs
```

(Inspect those scripts for the exact flag; the goal is to have a fresh `logs/<timestamp>/logfs/` directory with `UefiLogN.txt`, `gbl-chainload_BootN.txt`, etc.)

- [ ] **Step 3: Verify the filename rename**

```bash
LATEST=$(ls -td logs/2026* | head -1)
ls "$LATEST/logfs/"
```

Expected: `gbl-chainload_Boot*.txt` files present. The old `GblChainload_Boot*.txt` should NOT appear in this run (old runs still have the old name — that's expected).

- [ ] **Step 4: Verify the verbose output is no longer mirrored**

```bash
grep -c 'DynamicPatch:' "$LATEST/logfs/UefiLog"*.txt
grep -c 'DynamicPatch:' "$LATEST/logfs/gbl-chainload_Boot"*.txt
```

Expected:
- `UefiLog*.txt`: **zero** `DynamicPatch:` matches (verbose no longer mirrored).
- `gbl-chainload_Boot*.txt`: non-zero matches (our stream carries them).

- [ ] **Step 5: Verify boundary markers ARE on UefiLog**

```bash
grep -E 'gbl-chainload (entered|exiting)' "$LATEST/logfs/UefiLog"*.txt
grep -E 'gbl-chainload (entered|exiting)' "$LATEST/logfs/gbl-chainload_Boot"*.txt
```

Expected: both grepped files contain at least one entered and one exiting marker. Boundary markers appear in BOTH streams by design.

- [ ] **Step 6: Verify the BDS prelude survives**

```bash
head -20 "$LATEST/logfs/UefiLog"*.txt
```

Expected: the head of UefiLog shows PBL/XBL/ABL/BDS-stage banners (or, at minimum, no `DynamicPatch:` or `LogFs:` lines from us at the top). If our content is still at the top, the level-gate isn't working — return to Task 5 and confirm the gate is on the write call, not a wrapper.

(If the test device is not accessible, log Steps 1–6 as deferred and note "manual on-device smoke pending" in the PR description. The host-side test from Task 8 still gates the PR for code-only review.)

---

## Task 10: Commit and PR

**Files:** stage and commit on `feature/cleanup-p1c-log-stream-split`.

- [ ] **Step 1: Review changes**

Run:
```bash
git status -s
git diff GblChainloadPkg/Library/LogFsLib/
```

Expected:
- ` M` on the file holding the renamed constant.
- ` M GblChainloadPkg/Library/LogFsLib/DebugSink.c`.
- ` M` on any application/source file where Task 6 promoted boundary markers.
- `?? tests/051_log_stream_split.sh`.

Confirm the per-write flush in `Mount.c` is *unchanged* — diff `Mount.c` if it appears in `git status`; if the only diff there is the filename-constant rename, that's fine. Any other change to flush logic is a regression — back out.

- [ ] **Step 2: Stage and commit**

```bash
git add GblChainloadPkg/Library/LogFsLib/ tests/051_log_stream_split.sh
# If Task 6 touched Application/ files:
git add GblChainloadPkg/Application/
git commit -m "$(cat <<'EOF'
LogFsLib: split log streams — verbose to gbl-chainload, errors to UefiLog

UefiLogN.txt is owned by QCOM BDS and carries the PBL→XBL→ABL prelude.
Previously DEBUG output was mirrored to both UefiLog and our private
stream, flushing the prelude out of the file. This change gates the
UefiLog write on EFI_D_ERROR (boundary markers promoted to that bit),
so UefiLog now keeps only ERROR-class lines plus the two
"gbl-chainload entered/exiting" tracers.

Also renames the private stream filename from GblChainload_BootN.txt
to gbl-chainload_BootN.txt to match repo casing convention.

Preserves the per-write flush contract from
memory/active_investigation_log_flush.md (canoe SimpleFS commits only at
ExitBootServices, which the fastboot-fallback path never reaches).

Adds tests/051_log_stream_split.sh as a host-side regression check on
the filename constant.

Refs: docs/superpowers/specs/2026-05-12-cleanup-phase-1-design.md
(Cleanup Phase 1, PR-c).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 3: Verify commit**

Run: `git log --oneline -2 && git show --stat HEAD`

Expected: one new commit on `feature/cleanup-p1c-log-stream-split` touching LogFsLib + tests + optionally Application/.

- [ ] **Step 4: Push**

Run: `git push -u origin feature/cleanup-p1c-log-stream-split`

- [ ] **Step 5: Open PR**

```bash
gh pr create --title "LogFsLib: split log streams (PR-c of cleanup-phase-1)" --body "$(cat <<'EOF'
## Summary
- DebugSink now gates UefiLog writes on `EFI_D_ERROR`; verbose lines go only to the gbl-chainload stream.
- Two boundary markers (`gbl-chainload entered` / `gbl-chainload exiting`) promoted to `EFI_D_ERROR` so they remain visible in UefiLog.
- Filename rename: `GblChainload_BootN.txt` → `gbl-chainload_BootN.txt`.
- Per-write flush contract from `memory/active_investigation_log_flush.md` preserved verbatim.
- Adds `tests/051_log_stream_split.sh` as a regression check on the filename constant.

Refs: `docs/superpowers/specs/2026-05-12-cleanup-phase-1-design.md`. Sibling PRs: `cleanup-p1a-recovery-fix-paths-doc`, `cleanup-p1b-deprecate-synth-graft-fastboot`.

## Test plan
- [ ] `tests/051_log_stream_split.sh` exits 0.
- [ ] `./scripts/build.sh --mode 0` and `--mode 1` build clean.
- [ ] `./scripts/build.sh --mode 1 --auto --debug --verbose` builds clean.
- [ ] On device: `fastboot stage dist/mode-1-auto-debug-verbose.efi && fastboot oem boot-efi`; after pulling logs, `gbl-chainload_BootN.txt` exists, `UefiLogN.txt` shows the BDS prelude at the top and contains zero `DynamicPatch:` lines, and `UefiLogN.txt` contains the entered/exiting boundary markers.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

- [ ] **Step 6: Confirm PR is open**

Run: `gh pr view`

Expected: base = `main`, head = `feature/cleanup-p1c-log-stream-split`.
