# Mode-1 ↔ Fakelocked-Stock Comparison — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** verify that mode-1 (patch9 active + grafted vbmeta footers) builds the same Root of Trust and vbmeta hash that the pure-stock fakelocked baseline produced. Output a findings doc with a concrete pass / fail / inconclusive verdict.

**Status:** completed (findings in `docs/re/fakelock-vs-debug-comparison.md`, carried forward in commit `8d779e6`).

**Architecture:** read-only RE work. Pull data from two existing captures, build a property table, diff bootloader_log around ROT/vbmeta/verifiedboot, interpret divergences as expected (patch9 changes verification path) vs anomalous (ROT bytes that shouldn't depend on our patches). No source code changes.

**Tech Stack:** `grep`, `diff`, `awk`, `python3` for any tabulation. Output is markdown in `docs/re/`.

**File structure:**
- `docs/re/mode-1-vs-fakelocked-stock-findings.md` — single output file.

**Inputs (already on disk):**
- **Mode-1 (A)**: `logs/20260510-150754_manual_manual_v2.0-plan1/`
- **Fakelocked + pure-stock (B)**: `~/gbl-chainload-dirty/logs/20260508-202149_manual_mode-fakelocked-stock-images_vf91f6f3/`
- **Stock baseline (C)**: `~/gbl-chainload-dirty/logs/20260508-020314_stock_fastboot_getvar_all/`

---

## Task 1: Initialize the findings doc + capture inventory

**Files:**
- Create: `docs/re/mode-1-vs-fakelocked-stock-findings.md`

- [ ] **Step 1: Verify all three captures exist**

```bash
cd /home/vivy/gbl-chainload
A=logs/20260510-150754_manual_manual_v2.0-plan1
B=~/gbl-chainload-dirty/logs/20260508-202149_manual_mode-fakelocked-stock-images_vf91f6f3
C=~/gbl-chainload-dirty/logs/20260508-020314_stock_fastboot_getvar_all
for d in "$A" "$B" "$C"; do
  echo "==== $d ===="
  ls "$d" 2>&1 | head
done
```

Expected: A and B each show `bootconfig bootloader_log cmdline device-tree.tar dmesg.txt getprop.boot.txt logfs recovery.props`. C shows `getvar-all.txt`.

If any is missing, document in the findings doc as "input unavailable" and proceed with what's present. Re-capture per the spec's contingency only if Task 4's verdict comes out "inconclusive due to missing inputs".

- [ ] **Step 2: Scaffold the findings doc**

Create `docs/re/mode-1-vs-fakelocked-stock-findings.md`:

```markdown
# Mode-1 vs Fakelocked-Stock — Findings

**Date:** 2026-05-10
**Verdict:** _pending — set after Task 4 completes_

## Captures used

| ID | Path | Build | Device state |
|----|------|-------|--------------|
| A | `logs/20260510-150754_manual_manual_v2.0-plan1/` | v2.0-plan3 (post-Plan-3, patch9 active) | Mode-1, custom recovery + dtbo with grafted vbmeta footers |
| B | `~/gbl-chainload-dirty/logs/20260508-202149_manual_mode-fakelocked-stock-images_vf91f6f3/` | f91f6f3 (pre-patch9) | Mode-fakelocked, pure stock images |
| C | `~/gbl-chainload-dirty/logs/20260508-020314_stock_fastboot_getvar_all/` | f91f6f3 | Stock fastboot, locked, untouched |

## Sections (filled by Tasks 2-4)

- [ ] Property comparison table
- [ ] Bootloader log diff
- [ ] Interpretation
- [ ] Verdict
```

- [ ] **Step 3: Commit**

```bash
cd /home/vivy/gbl-chainload
git add docs/re/mode-1-vs-fakelocked-stock-findings.md
git commit -m "RE: scaffold mode-1 vs fakelocked-stock findings doc"
```

---

## Task 2: Property comparison table

**Files:**
- Modify: `docs/re/mode-1-vs-fakelocked-stock-findings.md`

Extract a fixed set of properties from each capture's `getprop.boot.txt`,
`bootconfig`, and `cmdline`, build a side-by-side table.

- [ ] **Step 1: Define the property keys**

Per the spec, the canonical key set:

```
ro.boot.verifiedbootstate
ro.boot.vbmeta.device_state
ro.boot.vbmeta.digest
ro.boot.vbmeta.size
ro.boot.vbmeta.hash_alg
ro.boot.vbmeta.avb_version
ro.boot.flash.locked
ro.boot.veritymode
androidboot.verifiedbootstate     (cmdline)
androidboot.vbmeta.public_key_digest
androidboot.boot_devices
androidboot.slot_suffix
```

- [ ] **Step 2: Extract per-capture values into staging files**

```bash
cd /home/vivy/gbl-chainload
A=logs/20260510-150754_manual_manual_v2.0-plan1
B=$HOME/gbl-chainload-dirty/logs/20260508-202149_manual_mode-fakelocked-stock-images_vf91f6f3
KEYS_PROP="ro.boot.verifiedbootstate ro.boot.vbmeta.device_state ro.boot.vbmeta.digest ro.boot.vbmeta.size ro.boot.vbmeta.hash_alg ro.boot.vbmeta.avb_version ro.boot.flash.locked ro.boot.veritymode"
KEYS_CMD="androidboot.verifiedbootstate androidboot.vbmeta.public_key_digest androidboot.boot_devices androidboot.slot_suffix"

for cap in A B; do
  case $cap in A) d=$A;; B) d=$B;; esac
  echo "==== $cap ===="
  for k in $KEYS_PROP; do
    val=$(grep -E "^\[$k\]" "$d/getprop.boot.txt" 2>/dev/null | sed -E "s/^\[$k\]: \[(.*)\]$/\1/")
    [[ -z "$val" ]] && val=$(grep -E "^$k " "$d/bootconfig" 2>/dev/null | head -1 | awk '{$1=""; print}')
    printf "  %-40s = %s\n" "$k" "${val:-(missing)}"
  done
  for k in $KEYS_CMD; do
    val=$(tr ' ' '\n' < "$d/cmdline" 2>/dev/null | grep -E "^$k=" | head -1 | cut -d= -f2-)
    [[ -z "$val" ]] && val=$(grep -E "^$k " "$d/bootconfig" 2>/dev/null | head -1 | awk '{$1=""; print}')
    printf "  %-40s = %s\n" "$k" "${val:-(missing)}"
  done
done
```

Save the output (you'll paste it into the doc in Step 3).

- [ ] **Step 3: Fill the property comparison table in the findings doc**

Open `docs/re/mode-1-vs-fakelocked-stock-findings.md` and replace
`- [ ] Property comparison table` with a real markdown table:

```markdown
## Property comparison table

| Key | A (mode-1, patch9) | B (fakelocked-stock, pre-patch9) | Same? |
|-----|-------------------|----------------------------------|-------|
| `ro.boot.verifiedbootstate` | _from A_ | _from B_ | yes/no |
| `ro.boot.vbmeta.device_state` | … | … | … |
| `ro.boot.vbmeta.digest` | … | … | … |
| `ro.boot.vbmeta.size` | … | … | … |
| `ro.boot.vbmeta.hash_alg` | … | … | … |
| `ro.boot.vbmeta.avb_version` | … | … | … |
| `ro.boot.flash.locked` | … | … | … |
| `ro.boot.veritymode` | … | … | … |
| `androidboot.verifiedbootstate` | … | … | … |
| `androidboot.vbmeta.public_key_digest` | … | … | … |
| `androidboot.boot_devices` | … | … | … |
| `androidboot.slot_suffix` | … | … | … |
```

Replace `…` with the actual values from Step 2's output.

For the `Same?` column: `yes` if the values match byte-for-byte; `no`
otherwise. For `slot_suffix`, differences are expected (depends on which
slot the device booted from); flag `expected`.

- [ ] **Step 4: Commit**

```bash
cd /home/vivy/gbl-chainload
git add docs/re/mode-1-vs-fakelocked-stock-findings.md
git commit -m "RE: mode-1 vs fakelocked-stock — property comparison table"
```

---

## Task 3: Bootloader log diff (ROT / vbmeta / verifiedboot)

**Files:**
- Modify: `docs/re/mode-1-vs-fakelocked-stock-findings.md`

ABL emits trace lines around Root of Trust computation, vbmeta hash
computation, and verified-boot state determination. Compare those
lines between A and B.

- [ ] **Step 1: Grep both captures for the relevant lines**

```bash
cd /home/vivy/gbl-chainload
A=logs/20260510-150754_manual_manual_v2.0-plan1
B=$HOME/gbl-chainload-dirty/logs/20260508-202149_manual_mode-fakelocked-stock-images_vf91f6f3

echo "==== A (mode-1) ===="
grep -iE "root of trust|RoT|vbmeta|verified.?boot|deviceunlock|locked|orange|yellow|green|red" "$A/bootloader_log" 2>/dev/null \
  | head -40
echo
echo "==== B (fakelocked-stock) ===="
grep -iE "root of trust|RoT|vbmeta|verified.?boot|deviceunlock|locked|orange|yellow|green|red" "$B/bootloader_log" 2>/dev/null \
  | head -40
```

- [ ] **Step 2: Annotate the diff in the findings doc**

Replace `- [ ] Bootloader log diff` with:

```markdown
## Bootloader log diff

### A (mode-1) — relevant excerpts

\`\`\`
(paste filtered A bootloader_log lines)
\`\`\`

### B (fakelocked-stock) — relevant excerpts

\`\`\`
(paste filtered B bootloader_log lines)
\`\`\`

### Notable differences

For each line that differs:
- **Topic** (e.g. ROT, vbmeta hash, verifiedboot state)
- **A says:** _excerpt_
- **B says:** _excerpt_
- **Why this differs:** _expected (patch9 changes path) / anomalous_
```

Fill in each `_…_` slot with the actual content from Step 1.

For a line to be "anomalous", it must represent state that *shouldn't*
depend on whether patch9 is active — for example, the device's
hardware-fused public key digest, or RoT bytes derived from immutable
device state. State that legitimately changes under patch9 (vbmeta
contents, verifiedbootstate, computed hashes that include the
grafted footer) is **expected**.

- [ ] **Step 3: Commit**

```bash
cd /home/vivy/gbl-chainload
git add docs/re/mode-1-vs-fakelocked-stock-findings.md
git commit -m "RE: mode-1 vs fakelocked-stock — bootloader log diff annotated"
```

---

## Task 4: Verdict

**Files:**
- Modify: `docs/re/mode-1-vs-fakelocked-stock-findings.md`

Decide pass / fail / inconclusive based on Tasks 2 + 3.

- [ ] **Step 1: Decision matrix**

Pull both filled sections (property table + bootloader log diff) into
view. Apply the rules:

- **PASS** — every divergence in the property table and bootloader log
  is explainable by patch9's intended behaviour (different vbmeta
  digest because of grafted footer; verifiedbootstate stays green
  because mode-1 fakes-lock; etc.). No anomalous lines that imply
  RoT / hardware-fused state is wrong.
- **FAIL** — at least one anomalous line where state that shouldn't
  depend on our patches has diverged. Concrete examples: A's
  `ro.boot.vbmeta.public_key_digest` differs from C's stock value
  (indicates patch9 may be feeding a wrong key); A reports
  `verifiedbootstate=orange` despite mode-1 trying to fakelocked-green.
- **INCONCLUSIVE** — captures lack the trace lines needed to decide.
  Specifically: B's bootloader_log doesn't contain any ROT / vbmeta /
  verifiedboot lines (older capture was sparse), OR property values
  legitimately differ in a way that we can't classify without more
  data. Spec's contingency: re-capture with pure stock images.

- [ ] **Step 2: Update the verdict line at the top of the doc**

Open the findings doc. Replace:

```markdown
**Verdict:** _pending — set after Task 4 completes_
```

with the verdict, and add an interpretation section:

```markdown
**Verdict:** PASS / FAIL / INCONCLUSIVE

## Interpretation

(1-2 paragraphs explaining the verdict, citing specific rows of the
property table and specific lines of the bootloader log diff.)

## If FAIL: next steps

(Concrete actions: which patch to revisit, which additional capture
would confirm.)

## If INCONCLUSIVE: contingency

(Per the spec: flash pure stock recovery + dtbo, boot with the older
mode-fakelocked .efi without patch9, capture via
test-device-manual.sh. Add the new capture as input D and re-run
this plan starting from Task 2 with A vs D.)
```

- [ ] **Step 3: Commit**

```bash
cd /home/vivy/gbl-chainload
git add docs/re/mode-1-vs-fakelocked-stock-findings.md
git commit -m "RE: mode-1 vs fakelocked-stock — verdict <PASS|FAIL|INCONCLUSIVE>"
```

(Replace `<...>` with the actual verdict in the commit message.)

---

## Self-Review

Checked against `docs/superpowers/specs/2026-05-10-mode-1-vs-fakelocked-comparison-design.md`:

- ✓ Sub-task 1 (inventory + property table) → Tasks 1 + 2.
- ✓ Sub-task 2 (bootloader log diff) → Task 3.
- ✓ Sub-task 3 (findings doc with verdict) → Tasks 1 (scaffold) + 4 (verdict).
- ✓ Contingency (re-capture if needed) → documented in Task 4 Step 2's INCONCLUSIVE path.

No placeholders. Capture paths (`A`, `B`, `C`) and the canonical key set
are consistent across Tasks 1, 2, 3, 4. Verdict values
(PASS/FAIL/INCONCLUSIVE) consistent in Task 4 Steps 1, 2, 3.

No source code touched; this plan produces a single findings doc. Per
the spec's "out of scope" — fixing whatever divergence is found is a
separate plan and is correctly left out of this plan.
