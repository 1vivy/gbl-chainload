# Cleanup Phase 1 — PR-b: Deprecate Synthesize/Graft Fastboot Surface Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the `oem synthesize-and-flash`, `oem graft-from-staged`, and `oem fix-vbmeta-footer` handlers (and the now-orphan helper code they pulled in) from `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c`; drop the matching FastbootMenu entries; delete `scripts/synthesize-vbmeta.py` (its only consumer); preserve `scripts/graft-vbmeta-from-stock.py` (PR-a's host fix path) and `tools/abl-patcher`, `tools/fv-unwrap` (Phase-2 worker tools).

**Architecture:** Forward-edit on the existing `fastboot/synthesize-and-flash` submodule branch — one new "remove surface" commit on edk2 (no rebase, no force-push; that's Phase-4 work). Main-repo branch carries the submodule bump + script deletion + memory-note banners + tests-Makefile audit. Acceptance gated by (a) a grep regression test in `tests/` that asserts the removed command literals are absent and (b) a full mode-0 + mode-1 build smoke. On-device verification: `fastboot stage dist/mode-1.efi && fastboot oem boot-efi` boots the test device.

**Tech Stack:** EDK2 (C), shell scripts, git submodule.

**Spec reference:** `docs/superpowers/specs/2026-05-12-cleanup-phase-1-design.md`, section "PR-b: Deprecate synthesize/graft fastboot surface (P2)".

**Safety note (CLAUDE.md):** This PR touches the fastboot surface but does not invoke any blocked fastboot operations. Device smoke-test path is `fastboot stage` + `fastboot oem boot-efi` only. The PreToolUse hook (`.claude/hooks/block-non-hlos-flash.py`) will block any accidental flash/unlock attempts — leave that hook intact.

---

## File Structure

**edk2 submodule (branch `fastboot/synthesize-and-flash`):**
- Modify: `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c` (remove three OEM-cmd handlers + their unreachable helpers).
- Modify: `edk2/QcomModulePkg/Library/FastbootLib/FastbootMenu.c` (or `.h` — locate in Task 3) (remove two menu entries).

**Main repo (branch `feature/cleanup-p1b-deprecate-synth-graft-fastboot`):**
- Modify: `edk2` (submodule pointer bump).
- Delete: `scripts/synthesize-vbmeta.py`.
- Modify (audit only, possibly no-op): `tests/patches/Makefile`.
- Modify: `memory/graft_at_natural_offset_wins.md` (historical banner).
- Modify: `memory/avb_constructed_verify_blocked.md` (historical banner).
- Create: `tests/050_no_synth_graft_surface.sh` (regression grep check).

---

## Task 1: Create feature branch and verify edk2 starting state

**Files:** none.

- [ ] **Step 1: Branch off main**

Run:
```bash
git checkout main
git checkout -b feature/cleanup-p1b-deprecate-synth-graft-fastboot
git status -s
```

Expected: `Switched to a new branch ...`. `git status -s` may show ` M edk2` (submodule pointer differs from main if you've run prior work locally) — that's expected; we'll deliberately bump it in Task 14.

- [ ] **Step 2: Verify edk2 submodule branch and HEAD**

Run:
```bash
git -C edk2 branch --show-current
git -C edk2 log --oneline -5
```

Expected:
- Branch: `fastboot/synthesize-and-flash`.
- HEAD has the three handler commits in history: `43e53788e2 ... oem graft-from-staged`, `1a41b8f43b ... factor synthesize core + add oem fix-vbmeta-footer`, `dbb36aca96 ... oem synthesize-and-flash`. If HEAD ≠ `43e53788e2` exactly, fetch + sync: `git -C edk2 fetch origin && git -C edk2 reset --hard origin/fastboot/synthesize-and-flash` and verify again.

- [ ] **Step 3: Confirm working tree of edk2 is clean**

Run: `git -C edk2 status -s`

Expected: empty output. If anything is staged or modified, STOP and resolve with the user before proceeding.

---

## Task 2: Map removal points in FastbootCmds.c

**Files:** Read-only: `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c`.

- [ ] **Step 1: Locate the three command-handler entries**

Run:
```bash
rg -n '"synthesize-and-flash"|"graft-from-staged"|"fix-vbmeta-footer"' edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c
```

Expected: each literal appears at least once (the command-name string used by the dispatch table). Note the line numbers — these anchor the command-table entries to remove. There may also be hits inside string-formatted INFO/OKAY responses inside handler bodies; treat the handler function as a deletable unit.

- [ ] **Step 2: Locate each handler function**

For each command, find its handler function (typical EDK2 pattern is `STATIC VOID CmdOem<Name> (CONST CHAR8 *arg, VOID *data, UINT32 sz)` or similar). Search:
```bash
rg -n 'CmdOemSynthesize|CmdOemGraft|CmdOemFixVbmeta|synthesize_and_flash|graft_from_staged|fix_vbmeta_footer' edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c
```

Expected: 3+ function definitions and the same names appearing in the command dispatch table (`{"oem synthesize-and-flash", CmdOemSynthesizeAndFlash}` or equivalent struct initializer).

- [ ] **Step 3: Identify the synthesize-core helpers**

Per spec, commit `1a41b8f43b` extracted a "synthesize core". Find its symbols:
```bash
git -C edk2 show 1a41b8f43b --stat
git -C edk2 show 1a41b8f43b -- QcomModulePkg/Library/FastbootLib/FastbootCmds.c | grep -E '^\+(STATIC )?(VOID|EFI_STATUS|UINTN) [A-Z]' | head -20
```

Expected: a handful of newly-introduced helper signatures (e.g. `SynthesizeVbmetaCore`, `WriteSynthesizedFooter`, etc.). Note these names — they're the orphan candidates to remove in Task 10.

- [ ] **Step 4: Record findings**

In your scratchpad (NOT a file), write down:
- Handler function names (3).
- Command-table entry line ranges (3).
- Synthesize-core helper names (variable count, from Step 3).
- Any string constants or `#define`s referenced only by the above (search via `rg <name> edk2/QcomModulePkg/Library/FastbootLib/`).

---

## Task 3: Map removal points in FastbootMenu

**Files:** Read-only: `edk2/QcomModulePkg/Library/FastbootLib/FastbootMenu.{c,h}`.

- [ ] **Step 1: Find the menu source**

Run: `ls edk2/QcomModulePkg/Library/FastbootLib/ | grep -i menu`

Expected: `FastbootMenu.c` (and possibly a `.h`).

- [ ] **Step 2: Locate the two entries from commit 06af9a026e**

Run:
```bash
git -C edk2 show 06af9a026e --stat
rg -n '"Fix recovery vbmeta footer"|"Escape"' edk2/QcomModulePkg/Library/FastbootLib/FastbootMenu.c
```

Expected:
- The commit shows the two added entries in the menu-table initializer.
- The `rg` returns at least the two literal strings and their menu-table rows.

- [ ] **Step 3: Confirm "Escape" is genuinely orphan-without-fix-footer**

Read the surrounding menu-table struct initializer. The spec says "Escape" was added in the same commit and only existed to bail out of the fix-footer flow. Verify by checking the entry's action — it should reference the fix-footer cancel path or no-op return.

If "Escape" has any other usage in the file (search `rg 'Escape' edk2/QcomModulePkg/Library/FastbootLib/FastbootMenu.c`), STOP and escalate — the spec assumption is wrong and we need user input on whether to keep it.

- [ ] **Step 4: Record findings**

Note the line ranges to remove (typically a struct-array entry — one row, brace-balanced).

---

## Task 4: Write the regression test

**Files:** Create: `tests/050_no_synth_graft_surface.sh`.

- [ ] **Step 1: Inspect existing test conventions**

Run: `ls tests/ | head -20 && head -30 tests/042_dynamic_patch_harness.sh`

Expected: shell tests with `set -euo pipefail`-style preamble. Match that style.

- [ ] **Step 2: Write the regression test**

Create `tests/050_no_synth_graft_surface.sh` with:

```bash
#!/usr/bin/env bash
# 050_no_synth_graft_surface.sh — regression check that the synthesize/graft
# fastboot surface stripped in cleanup-phase-1 PR-b stays gone.
#
# Asserts the three removed command literals are absent from edk2 source.
# Memory notes and docs may still mention them as history; only the
# edk2 source tree (and main-repo build glue) is searched.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

fail=0
for needle in 'synthesize-and-flash' 'graft-from-staged' 'fix-vbmeta-footer'; do
  hits=$(rg -n --no-heading -g '!docs' -g '!memory' -g '!.re-notes' \
              -g '!tests/050_no_synth_graft_surface.sh' \
              -- "$needle" "$ROOT/edk2/QcomModulePkg/Library/FastbootLib/" \
              "$ROOT/scripts/" 2>/dev/null || true)
  if [ -n "$hits" ]; then
    echo "FAIL: '$needle' still present in code paths:" >&2
    echo "$hits" >&2
    fail=1
  fi
done

if [ "$fail" -ne 0 ]; then
  exit 1
fi
echo "OK: synthesize/graft fastboot surface absent from code paths."
```

- [ ] **Step 3: Make it executable**

Run: `chmod +x tests/050_no_synth_graft_surface.sh`

- [ ] **Step 4: Run the test — expect FAIL**

Run: `tests/050_no_synth_graft_surface.sh`

Expected: exit code 1, with FAIL lines for each of the three needles (handlers and possibly the host script). Capture stderr to confirm. If the test PASSES at this point, the surface is already gone and PR-b's code work is unnecessary — STOP and escalate to the user.

---

## Task 5: Remove the `oem synthesize-and-flash` handler

**Files:** Modify: `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c`.

- [ ] **Step 1: Open the file at the handler**

Use the line numbers from Task 2 Step 2. Locate `CmdOemSynthesizeAndFlash` (or the actual function name from your scratchpad).

- [ ] **Step 2: Remove the function definition**

Delete the entire function body, including its leading `STATIC` modifiers and any block comment immediately preceding it that describes only this handler.

- [ ] **Step 3: Remove the command-table entry**

Locate the dispatch-table struct initializer for `"oem synthesize-and-flash"` (typically a row like `{"synthesize-and-flash", CmdOemSynthesizeAndFlash, "..."}` inside a `FASTBOOT_OEM_CMD_TABLE[]` or similarly-named array). Delete the entire row, including the trailing comma.

- [ ] **Step 4: Verify the file still compiles syntactically**

Run: `rg -n 'CmdOemSynthesizeAndFlash' edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c`

Expected: zero hits. If any remain, you missed a declaration; remove it.

---

## Task 6: Remove the `oem graft-from-staged` handler

**Files:** Modify: `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c`.

- [ ] **Step 1: Locate the handler**

Use Task 2's findings. The handler is typically `CmdOemGraftFromStaged` (from commit `43e53788e2`).

- [ ] **Step 2: Delete function body**

Same procedure as Task 5 Step 2 — full function + any single-handler block comment.

- [ ] **Step 3: Delete command-table entry**

Remove the `"graft-from-staged"` row from the dispatch table.

- [ ] **Step 4: Verify**

Run: `rg -n 'CmdOemGraftFromStaged|graft-from-staged' edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c`

Expected: zero hits.

---

## Task 7: Remove the `oem fix-vbmeta-footer` handler

**Files:** Modify: `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c`.

- [ ] **Step 1: Locate the handler**

Typically `CmdOemFixVbmetaFooter` (from commit `1a41b8f43b`).

- [ ] **Step 2: Delete function body + command-table entry**

Same procedure.

- [ ] **Step 3: Verify**

Run: `rg -n 'CmdOemFixVbmetaFooter|fix-vbmeta-footer' edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c`

Expected: zero hits.

---

## Task 8: Remove FastbootMenu entries

**Files:** Modify: `edk2/QcomModulePkg/Library/FastbootLib/FastbootMenu.c`.

- [ ] **Step 1: Delete the "Fix recovery vbmeta footer" menu row**

Use Task 3's line ranges. Remove the struct-array entry (one row, brace-balanced).

- [ ] **Step 2: Delete the "Escape" menu row**

Same procedure.

- [ ] **Step 3: Check menu-row count**

If the menu table has a length sentinel (e.g., `sizeof(menu_table)/sizeof(menu_table[0])`), it'll re-derive automatically. If there's a hard-coded `#define MENU_ENTRY_COUNT`, decrement it by 2.

Run: `rg -n 'MENU_ENTRY_COUNT|kMenuEntries|N_MENU' edk2/QcomModulePkg/Library/FastbootLib/`

Expected: either no hits (auto-sized) or hits that you've decremented.

- [ ] **Step 4: Verify**

Run:
```bash
rg -n '"Fix recovery vbmeta footer"|"Escape"' edk2/QcomModulePkg/Library/FastbootLib/FastbootMenu.c
```

Expected: zero hits.

---

## Task 9: Remove now-unreachable synthesize-core helpers

**Files:** Modify: `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c`.

- [ ] **Step 1: List candidate orphans**

From Task 2 Step 3 you have the helper names introduced by commit `1a41b8f43b`. For each one, check if anything still references it:

```bash
for helper in <NameA> <NameB> <NameC>; do
  echo "=== $helper ==="
  rg -n "\b$helper\b" edk2/QcomModulePkg/Library/FastbootLib/
done
```

Expected: zero remaining references means orphan → delete. If references remain (e.g., the helper is shared with `oem vbmeta-status`), keep it.

- [ ] **Step 2: Delete confirmed orphans**

For each helper with zero remaining references, delete its function definition (and any forward declaration).

- [ ] **Step 3: Sweep for unused static globals**

Run: `rg -n '^STATIC (CONST )?(CHAR8|UINT8|UINTN|EFI_STATUS) [a-zA-Z_]+ *[=;]' edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c | head -30`

For each STATIC-scoped global, check usage with `rg -n '\b<name>\b' edk2/QcomModulePkg/Library/FastbootLib/`. Delete any with zero references outside their own definition. Be conservative — if a name appears unused but is reasonable to keep (e.g., a vbmeta-status helper), leave it.

- [ ] **Step 4: Verify nothing in FastbootCmds.c references the removed handlers**

Run: `rg -n 'Synthesize|GraftFromStaged|FixVbmetaFooter|synth_|fix_vbmeta' edk2/QcomModulePkg/Library/FastbootLib/`

Expected: zero hits (or only comments that mention them historically — if so, delete those comment lines too).

---

## Task 10: Build smoke — mode-0 and mode-1

**Files:** none (build output goes to `Build/` and `dist/`).

- [ ] **Step 1: Clean build mode-0**

Run: `./scripts/build.sh --mode 0`

Expected: build completes, `dist/mode-0.efi` updated. If the build fails with linker errors (undefined references), an orphan helper still references one of the removed functions — go back to Task 9 and clean further.

- [ ] **Step 2: Clean build mode-1**

Run: `./scripts/build.sh --mode 1`

Expected: build completes, `dist/mode-1.efi` updated.

- [ ] **Step 3: Build the dev-capture variant**

Run: `./scripts/build.sh --mode 1 --auto --debug --verbose`

Expected: build completes, `dist/mode-1-auto-debug-verbose.efi` updated.

- [ ] **Step 4: Sanity-check artifact sizes**

Run: `ls -la dist/`

Expected: `mode-1.efi` is smaller than the pre-PR-b baseline (~569 KiB before; should drop by a few KiB after removing the handlers + helpers). If it grew, you accidentally added rather than removed code — review the diff.

---

## Task 11: Partial verification — edk2 surface is clean

**Files:** none. Verification only.

(NOTE: We deliberately do not run the full `tests/050_no_synth_graft_surface.sh` here. That test also scans `scripts/`, and `scripts/synthesize-vbmeta.py` is still present at this point — it gets deleted in Task 13. The full test runs as Task 13's final step.)

- [ ] **Step 1: edk2-scoped regression grep**

Run: `rg -n 'synthesize-and-flash|graft-from-staged|fix-vbmeta-footer' edk2/QcomModulePkg/Library/FastbootLib/`

Expected: **zero hits**. If anything matches, return to Task 5/6/7/9 and finish removing it.

- [ ] **Step 2: edk2-scoped helper-orphan grep**

Run: `rg -n 'CmdOemSynthesize|CmdOemGraft|CmdOemFixVbmeta' edk2/QcomModulePkg/Library/FastbootLib/`

Expected: zero hits.

- [ ] **Step 3: Menu entries gone**

Run: `rg -n '"Fix recovery vbmeta footer"|"Escape"' edk2/QcomModulePkg/Library/FastbootLib/FastbootMenu.c`

Expected: zero hits.

---

## Task 12: Commit on edk2 submodule

**Files:** commit inside the `edk2` submodule.

- [ ] **Step 1: Review the diff**

Run:
```bash
git -C edk2 diff --stat
git -C edk2 diff QcomModulePkg/Library/FastbootLib/FastbootCmds.c | head -80
```

Expected: deletions only (or near-only) in `FastbootCmds.c` and `FastbootMenu.c`. No additions to other files. If anything else changed, revert it: `git -C edk2 checkout -- <unrelated-file>`.

- [ ] **Step 2: Stage and commit**

Run:
```bash
git -C edk2 add QcomModulePkg/Library/FastbootLib/FastbootCmds.c QcomModulePkg/Library/FastbootLib/FastbootMenu.c
git -C edk2 commit -m "$(cat <<'EOF'
FastbootCmds+Menu: remove synthesize/graft/fix-vbmeta-footer surface

Drops three OEM commands and two FastbootMenu entries that served as
on-device experiments for the recovery-vbmeta graft path. The host
script scripts/graft-vbmeta-from-stock.py already implements the
working path; the Phase-2 on-device module will replace the fastboot
variants entirely.

Removed:
- CmdOemSynthesizeAndFlash (oem synthesize-and-flash)
- CmdOemGraftFromStaged    (oem graft-from-staged)
- CmdOemFixVbmetaFooter    (oem fix-vbmeta-footer)
- FastbootMenu entries: "Fix recovery vbmeta footer", "Escape"
- Associated synthesize-core helpers no longer referenced.

Preserved:
- oem boot-efi              (the only sanctioned non-flash test path)
- oem vbmeta-status         (read-only diagnostic)
- getvar vbmeta:<p>:expected, getvar mode/build-date/build-flags

Refs: docs/superpowers/specs/2026-05-12-cleanup-phase-1-design.md
(Cleanup Phase 1, PR-b).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 3: Note the new edk2 commit SHA**

Run: `git -C edk2 log --oneline -1`

Expected: a new commit on `fastboot/synthesize-and-flash`. Copy its SHA — you'll reference it in Task 14.

---

## Task 13: Delete `scripts/synthesize-vbmeta.py` and re-test

**Files:** Delete: `scripts/synthesize-vbmeta.py`.

- [ ] **Step 1: Pre-flight: confirm graft-vbmeta-from-stock.py is present and survives**

Run: `ls scripts/synthesize-vbmeta.py scripts/graft-vbmeta-from-stock.py`

Expected: both files present. If `graft-vbmeta-from-stock.py` is missing, STOP — it's the PR-a host fix path and must not be deleted.

- [ ] **Step 2: Search for callers of synthesize-vbmeta.py**

Run:
```bash
rg -l 'synthesize-vbmeta' --no-ignore --hidden -g '!.git' .
```

Expected callers: this plan, the spec, the recovery doc, memory notes — all documentation. Code callers should be limited to `scripts/synthesize-vbmeta.py` itself and possibly `tests/patches/Makefile`. If a Python helper or shell wrapper imports it, capture the path; Task 16 will resolve.

- [ ] **Step 3: Delete the file**

Run: `git rm scripts/synthesize-vbmeta.py`

(Use `git rm` since the file IS tracked, unlike the recovery doc in PR-a.)

Expected: `rm 'scripts/synthesize-vbmeta.py'`.

- [ ] **Step 4: Re-run the regression test**

Run: `tests/050_no_synth_graft_surface.sh`

Expected: exit code 0, output `OK: ...`. If FAIL: a non-code caller still references the script — fix per Task 16.

---

## Task 14: Bump edk2 submodule pointer

**Files:** Modify: `edk2` (submodule reference in main repo).

- [ ] **Step 1: Stage the submodule pointer**

Run:
```bash
git add edk2
git status -s edk2
```

Expected: ` M edk2` in the index column → staged.

- [ ] **Step 2: Verify the bump points at the new edk2 commit**

Run: `git diff --cached edk2`

Expected: output like `-Subproject commit <old SHA>` and `+Subproject commit <new SHA from Task 12 Step 3>`. The new SHA must match.

---

## Task 15: Audit and update `tests/patches/Makefile`

**Files:** Modify (or leave alone): `tests/patches/Makefile`.

- [ ] **Step 1: Search for references to synthesize-vbmeta**

Run: `rg -n 'synthesize-vbmeta|synthesize_vbmeta' tests/`

Expected: either zero hits (no-op) or hits in `tests/patches/Makefile` that need editing.

- [ ] **Step 2: If hits exist, remove them**

Open `tests/patches/Makefile` at the reported line. Remove the relevant rule, target, or variable assignment that references `synthesize-vbmeta.py`. If a target is purely a wrapper around the deleted script, remove the entire target including its prerequisites.

- [ ] **Step 3: If no hits, document the no-op in the PR description**

Note in your scratchpad: "tests/patches/Makefile audit: no references found, no edit required."

- [ ] **Step 4: If edited, run the test harness to confirm nothing broke**

Run: `make -C tests/patches help 2>/dev/null || make -C tests/patches 2>/dev/null || true; tests/042_dynamic_patch_harness.sh || true`

Expected: no errors from missing-target or undefined-variable. (`|| true` is intentional — these may be CI-gated; do not block PR-b on them.)

---

## Task 16: Update memory notes with historical banners

**Files:**
- Modify: `memory/graft_at_natural_offset_wins.md` (one-line banner at top).
- Modify: `memory/avb_constructed_verify_blocked.md` (one-line banner at top).

- [ ] **Step 1: Add banner to `graft_at_natural_offset_wins.md`**

Open the file. The first line after the YAML frontmatter (or the first H1 if no frontmatter) gets a preceding blockquote:

```markdown
> **Historical (2026-05-12):** Confirmed on infiniti. Fastboot surface removed in Cleanup Phase 1 PR-b; technique lives on in `scripts/graft-vbmeta-from-stock.py` and the Phase-2 on-device recovery-graft module.

```

(Insert blockquote + blank line BEFORE the first heading, but AFTER any YAML frontmatter — the frontmatter must remain valid.)

- [ ] **Step 2: Verify frontmatter intact**

Run: `head -10 memory/graft_at_natural_offset_wins.md`

Expected: if YAML frontmatter exists (`---` delimited), it sits at the top and the banner appears after the closing `---`.

- [ ] **Step 3: Add banner to `avb_constructed_verify_blocked.md`**

Same pattern:

```markdown
> **Historical (2026-05-12):** Resolved by graft path (see `graft_at_natural_offset_wins.md`); kept here for context on why constructed verify was abandoned.

```

- [ ] **Step 4: Verify both banners present**

Run: `rg -n 'Historical \(2026-05-12\)' memory/`

Expected: exactly two hits.

---

## Task 17: Full acceptance verification

**Files:** none (verification only).

- [ ] **Step 1: Run the regression test**

Run: `tests/050_no_synth_graft_surface.sh`

Expected: exit 0, `OK: ...`.

- [ ] **Step 2: Confirm mode-0 and mode-1 still build**

Run:
```bash
./scripts/build.sh --mode 0
./scripts/build.sh --mode 1
```

Expected: both succeed. Artifact mtimes update.

- [ ] **Step 3: Acceptance grep — full repo sweep**

Run:
```bash
rg -l 'synthesize-and-flash|graft-from-staged|fix-vbmeta-footer' --no-ignore --hidden -g '!.git'
```

Expected output paths fall under ONLY: `docs/`, `memory/`, `.re-notes/`, `tests/050_no_synth_graft_surface.sh` (which mentions them as the regression strings), and this plan file. ZERO hits under `edk2/QcomModulePkg/Library/FastbootLib/`, `scripts/`, `GblChainloadPkg/`, or `tests/patches/`.

- [ ] **Step 4: Confirm preserved diagnostic surface is intact**

Run: `rg -n '"boot-efi"|"vbmeta-status"|"vbmeta:"' edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c | head -20`

Expected: each of `boot-efi`, `vbmeta-status`, and `vbmeta:` (the getvar prefix) still present. If any is missing, you over-deleted; revert and retry from Task 5.

- [ ] **Step 5 (manual, on-device): smoke-test stage + boot-efi**

This step requires the test device. From a shell with the device in fastboot:
```bash
fastboot stage dist/mode-1.efi
fastboot oem boot-efi
```

Expected: device boots Android with KM 0x208 green (same behavior as pre-PR-b). If the device fails to boot, CAPTURE LOGS (`logs/<timestamp>` directory or via `device-monitor.sh`) and STOP. The most likely cause is an over-deletion of a helper that was actually still referenced by `oem boot-efi`.

(If the test device is not accessible at plan-execution time, log this step as deferred and note in the PR description that manual on-device smoke-test is pending.)

---

## Task 18: Commit on main repo

**Files:** stage and commit on `feature/cleanup-p1b-deprecate-synth-graft-fastboot`.

- [ ] **Step 1: Review staged + unstaged changes**

Run:
```bash
git status -s
git diff --cached
```

Expected staged:
- ` M edk2` (submodule bump).

Expected unstaged:
- `D  scripts/synthesize-vbmeta.py` (already deleted via `git rm`; should already be staged).
- ` M memory/graft_at_natural_offset_wins.md`.
- ` M memory/avb_constructed_verify_blocked.md`.
- `?? tests/050_no_synth_graft_surface.sh`.
- Possibly ` M tests/patches/Makefile` (if Task 15 edited it).

- [ ] **Step 2: Stage the remaining changes**

Run:
```bash
git add edk2 scripts/synthesize-vbmeta.py memory/graft_at_natural_offset_wins.md memory/avb_constructed_verify_blocked.md tests/050_no_synth_graft_surface.sh
# Add the Makefile only if Task 15 edited it:
if ! git diff --quiet tests/patches/Makefile 2>/dev/null; then
  git add tests/patches/Makefile
fi
```

- [ ] **Step 3: Verify staging**

Run: `git status -s`

Expected: every relevant file shows in the index column (`M `, `A `, or `D `). Working-tree column should be empty for all of them. No stray ` M edk2/...` (that means submodule has uncommitted internal changes — return to Task 12).

- [ ] **Step 4: Commit**

Run:
```bash
git commit -m "$(cat <<'EOF'
cleanup-phase-1 PR-b: deprecate synthesize/graft fastboot surface

Removes on-device fastboot experiments around recovery vbmeta in favor
of the host path (scripts/graft-vbmeta-from-stock.py) and the upcoming
Phase-2 on-device recovery-graft module.

edk2 (submodule bump on fastboot/synthesize-and-flash):
- FastbootCmds: drops oem synthesize-and-flash, oem graft-from-staged,
  oem fix-vbmeta-footer, and their now-unreferenced helpers.
- FastbootMenu: drops "Fix recovery vbmeta footer" and "Escape" entries.
- Preserves oem boot-efi, oem vbmeta-status, getvar vbmeta:<p>:expected,
  getvar mode/build-date/build-flags.

Main repo:
- Deletes scripts/synthesize-vbmeta.py (last consumer gone).
- Keeps scripts/graft-vbmeta-from-stock.py (host fix path documented in PR-a).
- Adds tests/050_no_synth_graft_surface.sh as a regression check.
- Adds historical banners to memory/graft_at_natural_offset_wins.md
  and memory/avb_constructed_verify_blocked.md.

Refs: docs/superpowers/specs/2026-05-12-cleanup-phase-1-design.md
(Cleanup Phase 1, PR-b). Prerequisite PR-a:
docs/re/recovery-normal-boot-fix-paths.md (separate PR).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 5: Verify commit**

Run: `git log --oneline -2 && git show --stat HEAD`

Expected: one new commit on `feature/cleanup-p1b-deprecate-synth-graft-fastboot`; `--stat` shows the submodule bump, the deleted script, the new test, and the memory-note edits.

---

## Task 19: Push and open PR

**Files:** none.

- [ ] **Step 1: Push the branch**

Run:
```bash
git push -u origin feature/cleanup-p1b-deprecate-synth-graft-fastboot
```

- [ ] **Step 2: Open PR**

Run:
```bash
gh pr create --title "cleanup-phase-1 PR-b: deprecate synthesize/graft fastboot surface" --body "$(cat <<'EOF'
## Summary
- Removes `oem synthesize-and-flash`, `oem graft-from-staged`, `oem fix-vbmeta-footer` from edk2 `FastbootCmds.c` plus their unreachable helpers; removes the two matching `FastbootMenu` entries.
- Deletes `scripts/synthesize-vbmeta.py` (its only consumer was the removed on-device path).
- Preserves `scripts/graft-vbmeta-from-stock.py` (PR-a's host fix path) and `tools/abl-patcher`, `tools/fv-unwrap` (Phase-2 worker tools).
- Adds `tests/050_no_synth_graft_surface.sh` as a regression check.
- Marks `memory/graft_at_natural_offset_wins.md` and `memory/avb_constructed_verify_blocked.md` historical.

Refs: `docs/superpowers/specs/2026-05-12-cleanup-phase-1-design.md`. Sibling: PR-a `cleanup-p1a-recovery-fix-paths-doc`.

## Test plan
- [ ] `tests/050_no_synth_graft_surface.sh` exits 0.
- [ ] `./scripts/build.sh --mode 0` builds clean.
- [ ] `./scripts/build.sh --mode 1` builds clean.
- [ ] `rg -l 'synthesize-and-flash|graft-from-staged|fix-vbmeta-footer' --no-ignore --hidden -g '!.git'` returns only docs/, memory/, .re-notes/, tests/050, and plan file.
- [ ] `rg -n '"boot-efi"|"vbmeta-status"' edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c` returns hits (preserved surface intact).
- [ ] On device: `fastboot stage dist/mode-1.efi && fastboot oem boot-efi` boots into Android, KM 0x208 green.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

- [ ] **Step 3: Confirm PR is open**

Run: `gh pr view`

Expected: PR title and body shown, base = `main`, head = `feature/cleanup-p1b-deprecate-synth-graft-fastboot`.
