# Cleanup Phase 1 — PR-a: Recovery Fix-Paths Doc Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Promote the recovery investigation note (`docs/re/aosp-early-avb-bootflow.md`) to an authoritative status doc named `recovery-normal-boot-fix-paths.md`, prepend the two committed fix paths (host script + Phase-2 module), cross-link from README and the AVB-input-facade doc, and mark the partition-read facade idea as superseded.

**Architecture:** Doc-only PR. One feature branch off `main`, one commit (file rename + content edits + two cross-link edits), one PR. No code touched. Acceptance verified by `rg` for dangling references and a manual read of the new file.

**Tech Stack:** Markdown only. `git mv` for the rename (file is currently untracked, so it's `mv` + `git add` — handled in Task 2).

**Spec reference:** `docs/superpowers/specs/2026-05-12-cleanup-phase-1-design.md`, section "PR-a: Recovery-failure decision doc (P1)".

---

## File Structure

- **Rename + edit:** `docs/re/aosp-early-avb-bootflow.md` → `docs/re/recovery-normal-boot-fix-paths.md` (currently untracked in working tree; one rename-and-stage operation).
- **Edit:** `README.md` (add one sentence + link to the new path).
- **Edit:** `docs/re/avb-input-facade.md` (prepend a status banner pointing at the new doc).
- **No code touched.**

---

## Task 1: Create feature branch

**Files:** none.

- [ ] **Step 1: Verify clean starting point on main**

Run: `git fetch origin && git status -s && git log --oneline -1`

Expected: `On branch main`, working tree may show the untracked recovery doc (`?? docs/re/aosp-early-avb-bootflow.md`) and possibly an `M edk2` submodule pointer. Both are intentional and unrelated to this PR. Last commit should be the spec commit on `feature/cleanup-phase-1-spec` or a merge of it; if you're on `main`, the spec may not be merged yet — that's fine, this PR is doc-only and does not depend on the spec landing.

- [ ] **Step 2: Branch off main**

Run:
```bash
git checkout main
git checkout -b feature/cleanup-p1a-recovery-fix-paths-doc
```

Expected: `Switched to a new branch 'feature/cleanup-p1a-recovery-fix-paths-doc'`.

- [ ] **Step 3: Verify the source doc is present in the working tree**

Run: `test -f docs/re/aosp-early-avb-bootflow.md && wc -l docs/re/aosp-early-avb-bootflow.md`

Expected: file exists, ~296 lines. If absent, the source doc is missing and this PR cannot proceed — STOP and escalate to the user.

---

## Task 2: Rename and add status banner

**Files:**
- Move: `docs/re/aosp-early-avb-bootflow.md` → `docs/re/recovery-normal-boot-fix-paths.md`
- Modify: top of the renamed file (prepend status banner above existing H1).

- [ ] **Step 1: Move the file**

Run: `mv docs/re/aosp-early-avb-bootflow.md docs/re/recovery-normal-boot-fix-paths.md`

(Plain `mv`, not `git mv`, because the file is currently untracked — `git mv` would refuse.)

Expected: silent success. Verify: `test -f docs/re/recovery-normal-boot-fix-paths.md && test ! -f docs/re/aosp-early-avb-bootflow.md`.

- [ ] **Step 2: Prepend the status banner above the existing H1**

Open `docs/re/recovery-normal-boot-fix-paths.md`. The current first line is:

```markdown
# AOSP Early-Stage AVB Bootflow Analysis
```

Replace lines 1–4 (existing title + investigation-date metadata) with:

```markdown
# Recovery Normal-Boot Fix Paths

**Status (2026-05-12):** Confirmed. Custom-recovery + normal-boot under mode-1 fails in AOSP `first_stage_init` (libfs_avb), which re-walks the vbmeta descriptor tree from disk after our shim has already lied to ABL's KM. The fix has to re-shape the on-disk recovery image *before* first-stage init reads it. Two committed paths: **host-side** `scripts/graft-vbmeta-from-stock.py` (already implemented, validated 2026-05-12 on infiniti), and **device-side** Phase-2 on-device recovery-graft module (TBD spec). The bootloader shim does not carry the fix because the read path it would need to intercept lives in userspace libfs_avb, outside our chainload domain.

**Original investigation date:** 2026-05-10. The analysis below documents the root cause; the "Committed fix paths" section names what we ship.
```

(The original `**Scenario:** ...` line on line 4 is preserved as the third paragraph of the front matter — leave it as-is below the new metadata.)

- [ ] **Step 3: Verify the new H1 lands cleanly**

Run: `head -8 docs/re/recovery-normal-boot-fix-paths.md`

Expected: title is `# Recovery Normal-Boot Fix Paths`, immediately followed by the Status banner, no orphan blank lines.

---

## Task 3: Add the "Committed fix paths" section

**Files:** Modify: `docs/re/recovery-normal-boot-fix-paths.md` (insert new section above existing "Recommended Mitigations").

- [ ] **Step 1: Locate the insertion point**

The existing doc has a section starting with:

```markdown
## Recommended Mitigations (Ranked by Likelihood)
```

Find the `---` horizontal rule **immediately preceding** that heading. Insert the new section between the previous content and that `---`.

- [ ] **Step 2: Insert the section**

Add the following block, ending with a `---` rule so the existing "Recommended Mitigations" section retains its leading rule:

```markdown
---

## Committed Fix Paths

These are the two paths we ship. The "Recommended Mitigations" section below is preserved for historical context — those options were considered as alternatives.

### Host path (available now)

```bash
scripts/graft-vbmeta-from-stock.py \
  --partition recovery \
  --in <custom>.img \
  --stock <stock-recovery>.img \
  --out <patched>.img
```

Pastes stock recovery vbmeta at `round_up(custom_image_size, 4 KiB)`. With patch10 in the boot path, ABL emits `verify_result_local=OK` for recovery and the slot-level recoverable error is caught by patch10 → final OK. Required for every custom-recovery flash until the device-side path lands.

### Device path (Phase 2, TBD)

On-device companion module that performs the same graft against the on-device stock vbmeta, automatically, post-recovery-flash. Lives next to the OTA-cached-ABL module. Spec TBD under Cleanup Phase 2.

```

(Note: the inner fenced code block uses four backticks if the outer block above uses three — adjust during the edit so the markdown nests correctly. Easiest is to keep the outer block as the literal markdown text and let the inner ```bash block stay as ```.)

- [ ] **Step 3: Verify the section renders**

Run: `grep -n 'Committed Fix Paths' docs/re/recovery-normal-boot-fix-paths.md`

Expected: exactly one hit, at a line number above the "Recommended Mitigations" heading line (also visible in the grep output below it if you broaden the regex). Run `grep -n '^## ' docs/re/recovery-normal-boot-fix-paths.md` to see the section ordering; "Committed Fix Paths" must appear before "Recommended Mitigations".

---

## Task 4: Annotate the superseded mitigation

**Files:** Modify: `docs/re/recovery-normal-boot-fix-paths.md` (in-place note under section (a)).

- [ ] **Step 1: Locate section (a)**

The existing doc has:

```markdown
### (a) **AVB Partition-Read Facade (MOST LIKELY TO WORK)**

**Approach:** Intercept libavb's `read_from_partition()` calls ...
```

- [ ] **Step 2: Insert a one-line "Why not pursued in shim" subsection**

Immediately after the section-(a) heading line and before the `**Approach:**` paragraph, insert:

```markdown
> **Why not pursued in shim (2026-05-12):** the read path lives in userspace libfs_avb, outside our chainload domain. Effectively superseded by the disk-side graft documented in "Committed Fix Paths" above. The facade idea remains noted here as a design alternative considered.

```

(Blockquote with a trailing blank line, so the existing `**Approach:**` paragraph stays separated.)

- [ ] **Step 3: Verify**

Run: `grep -n 'Why not pursued in shim' docs/re/recovery-normal-boot-fix-paths.md`

Expected: exactly one hit, between the `### (a)` heading and the `**Approach:**` line.

---

## Task 5: Cross-link from README

**Files:** Modify: `README.md` (one sentence added to the Status section).

- [ ] **Step 1: Locate the Status section**

Open `README.md`. The Status section is lines 5–9:

```markdown
## Status

v2 architecture in flight. See `docs/superpowers/specs/` for the design and `docs/superpowers/plans/` for the implementation plan series.

Working artifacts: `dist/mode-0.efi` (pass-through observation build) and `dist/mode-1.efi` (protocol-hook fakelock via `QCOM_VERIFIEDBOOT_PROTOCOL` mutation; KM/Oplus see locked/green when stock images verify cleanly).
```

- [ ] **Step 2: Append a recovery sentence after the "Working artifacts" paragraph**

Insert (as a new paragraph after the "Working artifacts" line):

```markdown
Mode-1 supports the "stock recovery + custom system" use case by default. If you flash a custom recovery, use `scripts/graft-vbmeta-from-stock.py` to graft stock vbmeta onto your custom image before flashing — see [`docs/re/recovery-normal-boot-fix-paths.md`](docs/re/recovery-normal-boot-fix-paths.md). A device-side companion module that automates this is Cleanup Phase 2 work.
```

- [ ] **Step 3: Verify**

Run: `grep -n 'recovery-normal-boot-fix-paths' README.md`

Expected: exactly one hit. Open `README.md` and confirm the new paragraph reads cleanly in the Status section.

---

## Task 6: Cross-link from avb-input-facade

**Files:** Modify: `docs/re/avb-input-facade.md` (prepend a banner above the existing H1).

- [ ] **Step 1: Read the current top of the file**

Run: `head -5 docs/re/avb-input-facade.md`

Expected first line: `# AVB input façade plan for recovery/dtbo embedded vbmeta`.

- [ ] **Step 2: Insert a banner above the H1**

Prepend (before existing line 1):

```markdown
> **Status (2026-05-12):** The partition-read façade idea in this doc did **not** graduate into shim code. The path that shipped is the disk-side graft (host script + Phase-2 device module) documented in [`recovery-normal-boot-fix-paths.md`](recovery-normal-boot-fix-paths.md). This doc is preserved as a design alternative considered.

```

(Blockquote + trailing blank line so the existing H1 stays on its own line.)

- [ ] **Step 3: Verify**

Run: `grep -n 'recovery-normal-boot-fix-paths' docs/re/avb-input-facade.md`

Expected: exactly one hit, above the existing H1.

---

## Task 7: Verify no dangling references and acceptance

**Files:** none (verification only).

- [ ] **Step 1: Scan for the old filename**

Run: `rg -l 'aosp-early-avb-bootflow' .` (from repo root)

Expected: **no output** (zero hits). If anything matches, open each hit and replace `aosp-early-avb-bootflow` with `recovery-normal-boot-fix-paths`. Common candidates: `docs/superpowers/specs/2026-05-12-cleanup-phase-1-design.md` references the rename target — those references are expected to use the new name already; if they don't, fix them. Re-run the `rg` until it returns zero.

- [ ] **Step 2: Confirm the new doc has all expected anchors**

Run: `grep -nE '^(##|###) ' docs/re/recovery-normal-boot-fix-paths.md | head -20`

Expected sections in order: existing "Executive Summary", existing "Early-AVB Call Chain ...", existing other sections, **then** "Committed Fix Paths", **then** "Recommended Mitigations (Ranked by Likelihood)". The Committed Fix Paths section must appear before Recommended Mitigations.

- [ ] **Step 3: Confirm README and facade-doc cross-links resolve**

Run:
```bash
grep -n 'recovery-normal-boot-fix-paths' README.md docs/re/avb-input-facade.md
```

Expected: exactly two hits, one per file.

---

## Task 8: Stage and commit

**Files:** all three modified files staged in a single commit.

- [ ] **Step 1: Review the diff (renamed file + two edits)**

Run:
```bash
git status -s
git diff -- README.md docs/re/avb-input-facade.md
```

Expected git status shows:
- `?? docs/re/recovery-normal-boot-fix-paths.md` (the renamed file, currently untracked)
- ` M README.md`
- ` M docs/re/avb-input-facade.md`

(The original `aosp-early-avb-bootflow.md` is no longer present, which is correct.)

- [ ] **Step 2: Stage**

Run:
```bash
git add docs/re/recovery-normal-boot-fix-paths.md README.md docs/re/avb-input-facade.md
```

Verify with `git status -s`: all three should show `A ` or `M ` in the index column.

- [ ] **Step 3: Commit**

Run:
```bash
git commit -m "$(cat <<'EOF'
docs: recovery normal-boot fix-paths status doc (PR-a of cleanup-phase-1)

Promotes the recovery investigation note to an authoritative status doc.

- Renames docs/re/aosp-early-avb-bootflow.md to
  docs/re/recovery-normal-boot-fix-paths.md.
- Adds a status banner naming the two committed fix paths:
  scripts/graft-vbmeta-from-stock.py (host, already shipping) and the
  Phase-2 on-device recovery-graft module (TBD).
- Adds a "Committed Fix Paths" section above the preserved
  "Recommended Mitigations" analysis; tags option (a)
  (AVB Partition-Read Facade) as superseded by the disk-side graft.
- Cross-links from README.md and docs/re/avb-input-facade.md.

Refs: docs/superpowers/specs/2026-05-12-cleanup-phase-1-design.md

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 4: Verify commit**

Run: `git log --oneline -1 && git show --stat HEAD`

Expected: one new commit on `feature/cleanup-p1a-recovery-fix-paths-doc`; the `--stat` shows the renamed doc (as create+delete on a single rename detection, or A+D pair depending on git's similarity threshold), plus changes to `README.md` and `docs/re/avb-input-facade.md`.

---

## Task 9: Push and open PR

**Files:** none.

- [ ] **Step 1: Push the branch**

Run:
```bash
git push -u origin feature/cleanup-p1a-recovery-fix-paths-doc
```

Expected: branch tracking set, push succeeds.

- [ ] **Step 2: Open PR**

Run:
```bash
gh pr create --title "docs: recovery normal-boot fix-paths status doc (PR-a of cleanup-phase-1)" --body "$(cat <<'EOF'
## Summary
- Renames `docs/re/aosp-early-avb-bootflow.md` → `docs/re/recovery-normal-boot-fix-paths.md`.
- Adds a status banner + "Committed Fix Paths" section naming host (`scripts/graft-vbmeta-from-stock.py`) and Phase-2 device-side paths.
- Cross-links from `README.md` and `docs/re/avb-input-facade.md`.
- Tags the AVB Partition-Read Facade idea as superseded by the disk-side graft.

Refs: `docs/superpowers/specs/2026-05-12-cleanup-phase-1-design.md` (Cleanup Phase 1 spec, PR-a section).

## Test plan
- [ ] `rg -l aosp-early-avb-bootflow .` returns zero hits.
- [ ] `grep -n 'recovery-normal-boot-fix-paths' README.md docs/re/avb-input-facade.md` returns one hit per file.
- [ ] Manual read: new doc reads cleanly, fix-paths section appears above recommended-mitigations.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

Expected: PR URL printed. Note the URL for the next sub-PR (PR-b will reference this PR in its commit message).

- [ ] **Step 3: Confirm PR is open**

Run: `gh pr view --web` (or just `gh pr view`).

Expected: PR title and body shown, base = `main`, head = `feature/cleanup-p1a-recovery-fix-paths-doc`.
