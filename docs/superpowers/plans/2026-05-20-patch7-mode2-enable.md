# patch7 mode-2 enable — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Unarchive patch7 (orange-state screen rewrite) in the oneplus OEM scope so mode-2 cached ABLs silence the orange-state warning, and widen the mode-2 ZIP's OEM detect to cover the full oplus family (oneplus / oppo / oplus / realme).

**Architecture:** Patch7 already exists in `oem/oneplus_canoe.c` under `#ifdef GBL_PATCH7_ENABLED`. We drop the ifdef so the patch is unconditionally registered in `kOemOneplusPatches[]`. No new scope, no per-patch mode mask, no new `--mode` CLI — the existing `--oem oneplus` opt-in already gates mode-2 application (mode-1 install never passes `--oem`). The mode-2 ZIP's `detect_oem` widens its case branch. Tests update to reflect patch7's live status; one already-marked TODO in `tests/host/083` is closed.

**Tech Stack:** C (host build of DynamicPatchLib via `__HOST_BUILD__`), POSIX shell (zip installer), make-driven host tests.

**Spec:** [`docs/superpowers/specs/2026-05-20-patch7-mode2-enable-design.md`](../specs/2026-05-20-patch7-mode2-enable-design.md)

---

## File Structure

- **Modify** `GblChainloadPkg/Library/DynamicPatchLib/oem/oneplus_canoe.c` — drop `#ifdef GBL_PATCH7_ENABLED`; patch7 entry becomes unconditional.
- **Modify** `tests/patches/test_patch7.c` — drop the "archived" comment; add a `kOemOneplusPatchesCount`/`kOemOneplusPatches[]` membership assertion. Keep the direct-call regression checks.
- **Modify** `tests/host/083_abl_patcher_oem.sh` — close the documented TODO: assert `patch7-orange-screen` appears in the `--oem oneplus --no-mode1` run's per-patch log lines, and does NOT appear in the plain (no `--oem`) run.
- **Modify** `zip/modes/mode-2-install.sh` — widen the OEM detect case branch to `*oneplus*|*oppo*|*oplus*|*realme*`.
- **No changes** to `PatchDesc.h`, `PatchTable.c`, `universal/universal.c`, `mode_1/mode_1.c`, `tools/abl-patcher/abl-patcher.c`, `tests/045_mode_taxonomy_lint.sh` (045 already greps for `patch7-orange-screen` in `oem/oneplus_canoe.c` — passes today, passes after).

---

## Task 0: Branch setup

**Goal:** Land changes on a dedicated feature branch off `main`, per the project's PR-only workflow.

**Files:** none (git only).

**Acceptance Criteria:**
- [ ] Working tree clean (or only contains the staged spec commit `dabd30d`)
- [ ] On a new branch `feat/patch7-mode2-enable` based on `main`
- [ ] The spec commit `dabd30d` is reachable from the new branch (cherry-pick or rebase)

**Verify:**
```
git rev-parse --abbrev-ref HEAD
# expected: feat/patch7-mode2-enable
git log --oneline main..HEAD
# expected: includes dabd30d "docs(spec): patch7 enable for mode-2"
```

**Steps:**

- [ ] **Step 1: Check current state and stash if needed**

```bash
git status
git log --oneline -5
```

The spec commit `dabd30d` currently sits on `spec/release-workflow`. We move it to its own branch.

- [ ] **Step 2: Create feature branch from main, cherry-pick the spec commit**

```bash
git fetch origin
git checkout -b feat/patch7-mode2-enable origin/main
git cherry-pick dabd30d
```

- [ ] **Step 3: Verify**

```bash
git rev-parse --abbrev-ref HEAD          # → feat/patch7-mode2-enable
git log --oneline main..HEAD             # → 1 commit, "docs(spec): patch7 enable for mode-2"
```

No commit in this task (cherry-pick handles it).

---

## Task 1: Unarchive patch7 in `oem/oneplus_canoe.c`

**Goal:** `kOemOneplusPatches[]` is a single-entry table containing the patch7 descriptor, with no `#ifdef GBL_PATCH7_ENABLED`.

**Files:**
- Modify: `GblChainloadPkg/Library/DynamicPatchLib/oem/oneplus_canoe.c`

**Acceptance Criteria:**
- [ ] No occurrences of `GBL_PATCH7_ENABLED` anywhere in the repo
- [ ] `kOemOneplusPatches[]` contains exactly one entry, named `patch7-orange-screen`, with `.Scope = SCOPE_OEM_ONEPLUS` and `.Mandatory = FALSE`
- [ ] `kOemOneplusPatchesCount` is `sizeof (kOemOneplusPatches) / sizeof (kOemOneplusPatches[0])`
- [ ] File-leading doc comment no longer says "archived from active mode-1 table"; describes patch7 as oem-scope, included automatically by `--oem oneplus` (host) and unconditionally by the EFI runtime aggregator
- [ ] `make -s -C tools/abl-patcher` rebuilds cleanly
- [ ] `make -s -C tests/patches` rebuilds cleanly (the existing `test_patch7` Makefile target already links `oem/oneplus_canoe.c`)
- [ ] `bash tests/045_mode_taxonomy_lint.sh` still passes (the lint already greps for `patch7-orange-screen` in this file)

**Verify:**
```
! grep -rn GBL_PATCH7_ENABLED GblChainloadPkg tools tests
make -s -C tools/abl-patcher
make -s -C tests/patches
bash tests/045_mode_taxonomy_lint.sh
```
Expected: `grep` exits non-zero (no matches); both `make` commands succeed; lint prints `ok 045_mode_taxonomy_lint`.

**Steps:**

- [ ] **Step 1: Rewrite `oneplus_canoe.c`**

Replace the entire file contents with:

```c
/** @file oneplus_canoe.c — OnePlus/Oppo/Realme (oplus / canoe) family OEM patches.

  ## Patch 7 — orange-state-screen + unlock-warning + 5-second boot-delay gate

  LinuxLoaderEntry guards an orange-state warning block with a CBZ that skips
  the block when the device is locked.  Rewriting that CBZ as an unconditional B
  always skips the block, regardless of lock state.

  Anchor: the 4 CSEL/AND bytes at the equivalent of infiniti:0x78EC, which
  immediately precede the CBZ.  The anchor is unique in the executable section
  and does not include the CBZ word itself, so patching is idempotent.

  Faithful port of gbl_root_canoe tools/patchlib.h:patch_orange_state_screen.
  Non-mandatory — cosmetic only; PATCH_MISS on non-matching ABLs is a clean
  no-op.

  Scope: SCOPE_OEM_ONEPLUS.  Selected at host build time by
  `abl-patcher --oem oneplus`, and aggregated automatically by the EFI
  runtime patch table (mode-1 fakelocks the orange-state code path so the
  rewrite is dead code there; mode-2 keeps ABL honest and needs the rewrite
  to silence the warning).
**/

#include "../../../Include/Library/PatchDesc.h"
#include "../Internal/ScanLib.h"
#include "../Internal/Encode.h"
#include "Signatures.h"

PATCH_OUTCOME
ApplyOrangeScreen (
  IN OUT UINT8  *Buf,
  IN     UINT32  Size
  )
{
  UINT32      AnchorOff;
  SCAN_RESULT R;

  R = ScanForBoundedSection (Buf, Size, /*ExecOnly=*/TRUE,
                             kPatch7AnchorPattern, NULL,
                             kPatch7AnchorPatternLen, &AnchorOff);
  if (R == SCAN_NOT_FOUND) return PATCH_MISS;
  if (R == SCAN_AMBIGUOUS)  return PATCH_AMBIGUOUS;
  if (R != SCAN_FOUND)      return PATCH_MISS;

  WriteInstrU32 (Buf, AnchorOff + kPatch7RewriteDelta, kPatch7BUnconditionalInsn);
  return PATCH_OK;
}

CONST PATCH_DESC kOemOneplusPatches[] = {
  {
    .Name      = "patch7-orange-screen",
    .Scope     = SCOPE_OEM_ONEPLUS,
    .Mandatory = FALSE,
    .Apply     = ApplyOrangeScreen,
  },
};

CONST UINTN kOemOneplusPatchesCount =
  sizeof (kOemOneplusPatches) / sizeof (kOemOneplusPatches[0]);
```

- [ ] **Step 2: Confirm no other references to the old macro**

```bash
grep -rn GBL_PATCH7_ENABLED GblChainloadPkg tools tests scripts
```
Expected: no output (exit 1).

- [ ] **Step 3: Build the host tool**

```bash
make -s -C tools/abl-patcher
```
Expected: rebuilds without warnings or errors.

- [ ] **Step 4: Build the existing patch tests**

```bash
make -s -C tests/patches
```
Expected: all `test_patchN` targets build. `test_patch7` will fail to PASS at this point because its `#include` comments still claim "archived" and there is no membership assertion yet — that's Task 2's job. Build success is what we check here.

Note: if `make` in `tests/patches` runs the tests as part of `all` and one fails, that's expected — we'll fix the test contents in Task 2. To build without running, use `make -s -C tests/patches test_patch7 test_patch1 test_patch10 test_patch6` (named targets, no `all:` recipe execution).

- [ ] **Step 5: Run the taxonomy lint**

```bash
bash tests/045_mode_taxonomy_lint.sh
```
Expected: `ok 045_mode_taxonomy_lint`.

- [ ] **Step 6: Commit**

```bash
git add GblChainloadPkg/Library/DynamicPatchLib/oem/oneplus_canoe.c
git commit -m "$(cat <<'EOF'
feat(patches): unarchive patch7 (orange-screen) in oneplus OEM scope

Drop GBL_PATCH7_ENABLED ifdef. Patch7 is now an unconditional entry in
kOemOneplusPatches[]. Mode-1 install does not pass --oem so its cached
ABL is unchanged; mode-2 install passes --oem oneplus and now applies
patch7 to silence the orange-state warning.

Patch7 is non-mandatory and idempotent — PATCH_MISS on non-matching
ABLs (e.g. unverified Realme/Oppo anchors) is a clean no-op.

See docs/superpowers/specs/2026-05-20-patch7-mode2-enable-design.md.
EOF
)"
```

```json:metadata
{"files": ["GblChainloadPkg/Library/DynamicPatchLib/oem/oneplus_canoe.c"], "verifyCommand": "! grep -rn GBL_PATCH7_ENABLED GblChainloadPkg tools tests && make -s -C tools/abl-patcher && bash tests/045_mode_taxonomy_lint.sh", "acceptanceCriteria": ["GBL_PATCH7_ENABLED removed from repo", "kOemOneplusPatches[] has 1 entry named patch7-orange-screen", "kOemOneplusPatchesCount uses sizeof()/sizeof()", "tools/abl-patcher rebuilds clean", "tests/patches rebuilds clean", "tests/045_mode_taxonomy_lint.sh passes"]}
```

---

## Task 2: Update `tests/patches/test_patch7.c` for the new wiring

**Goal:** The host test reflects that patch7 is no longer archived. Comment is updated; a new assertion confirms the patch is registered in `kOemOneplusPatches[]`. All existing anchor / pre-patch / apply / post-patch / B-target / idempotency checks keep passing on the infiniti fixture.

**Files:**
- Modify: `tests/patches/test_patch7.c`

**Acceptance Criteria:**
- [ ] File comment no longer says "archived from the active mode-1 aggregator table" — describes the test as a regression check for patch7's logic AND aggregator membership
- [ ] Adds a section that asserts `kOemOneplusPatchesCount >= 1` and that some entry in `kOemOneplusPatches[]` has `.Name == "patch7-orange-screen"` and `.Apply` non-null
- [ ] Membership check runs BEFORE the fixture-dependent checks, so it executes even when the infiniti fixture is absent (SKIP)
- [ ] All existing checks (anchor uniqueness, pre-patch CBZ, PATCH_OK, post-patch B insn, B-target == CBZ-target, idempotency) still pass
- [ ] When fixture is absent, prints `ok patch7 table membership` + `SKIP: test_patch7 — infiniti fixture ...` and exits 0

**Verify:**
```
make -s -C tests/patches test_patch7 && cd tests/patches && ./test_patch7
```
Expected: with fixture present → all `ok` lines + `ALL PASS`. Without fixture → `ok patch7 table membership` + `SKIP: ...` + exit 0.

**Steps:**

- [ ] **Step 1: Edit the file header comment**

Replace lines 1-5 of `tests/patches/test_patch7.c` with:

```c
/* Host test for patch7 (orange-screen) against the infiniti fixture.
   Verifies BOTH the patch's byte-level logic (anchor uniqueness, CBZ→B
   rewrite, target preservation, idempotency) AND its registration in the
   active OEM aggregator table `kOemOneplusPatches[]`.  The membership
   check runs first so it executes even when the infiniti fixture is
   absent (SKIP path).  */
```

- [ ] **Step 2: Declare the OEM table externs**

After the existing `extern PATCH_OUTCOME ApplyOrangeScreen ...` line, add:

```c
extern CONST PATCH_DESC  kOemOneplusPatches[];
extern CONST UINTN       kOemOneplusPatchesCount;
```

- [ ] **Step 3: Add the membership check at the top of `main()`**

Right at the start of `main (void)` (before `UINT32 size = 0;`), insert:

```c
  /* --- 0. Table membership (runs regardless of fixture presence) ---------- */
  assert (kOemOneplusPatchesCount >= 1 && "kOemOneplusPatches must contain patch7");
  int found_patch7 = 0;
  for (UINTN k = 0; k < kOemOneplusPatchesCount; ++k) {
    if (kOemOneplusPatches[k].Name != NULL
        && kOemOneplusPatches[k].Apply != NULL
        && 0 == __builtin_strcmp ((const char *)kOemOneplusPatches[k].Name,
                                  "patch7-orange-screen")) {
      found_patch7 = 1;
      break;
    }
  }
  assert (found_patch7 && "patch7-orange-screen not found in kOemOneplusPatches[]");
  printf ("ok patch7 table membership\n");
```

If `<string.h>` isn't already included, also add `#include <string.h>` at the top (then `strcmp` instead of `__builtin_strcmp`). Check the existing includes — the file already has `<stdio.h>` `<stdlib.h>` `<assert.h>`. Use `__builtin_strcmp` to avoid pulling a new header.

- [ ] **Step 4: Build and run**

```bash
make -s -C tests/patches test_patch7
( cd tests/patches && ./test_patch7 )
```
Expected (with fixture):
```
ok patch7 table membership
ok patch7 anchor uniqueness (off=0x78d8)
ok patch7 pre-patch CBZ word 0x3400046a (W10)
ok patch7 PATCH_OK
ok patch7 rewrite vs kPatch7BUnconditionalInsn (0x14000023)
ok patch7 B target matches CBZ target (0x797c)
ok patch7 idempotency
ALL PASS
```

Without fixture:
```
ok patch7 table membership
SKIP: test_patch7 — infiniti fixture .../LinuxLoader_infiniti.efi not present
```

- [ ] **Step 5: Run the full patches harness**

```bash
make -s -C tests/patches
```
Expected: every test in `TESTS := test_patch1 test_patch7 test_patch10 test_patch6` builds and reports `PASS` (or `SKIP` for fixture-dependent tests).

- [ ] **Step 6: Commit**

```bash
git add tests/patches/test_patch7.c
git commit -m "$(cat <<'EOF'
test(patches): test_patch7 asserts kOemOneplusPatches[] membership

Update the test comment (patch7 is no longer archived) and add a
table-membership check that runs regardless of fixture presence.
Existing byte-level checks unchanged.
EOF
)"
```

```json:metadata
{"files": ["tests/patches/test_patch7.c"], "verifyCommand": "make -s -C tests/patches test_patch7 && (cd tests/patches && ./test_patch7)", "acceptanceCriteria": ["File comment updated (no 'archived' language)", "Membership assertion runs before fixture-dependent checks", "Existing anchor/CBZ/idempotency checks pass on fixture", "SKIP path still emits 'ok patch7 table membership' before SKIP"]}
```

---

## Task 3: Extend `tests/host/083_abl_patcher_oem.sh` to assert patch7 applied

**Goal:** Close the TODO already documented in `tests/host/083`:
> "Once GBL_PATCH7_ENABLED is enabled, add a positive grep for that patch name in the --oem oneplus run to close this coverage gap."

The test now asserts patch7 is applied in the `--oem oneplus --no-mode1` run and absent in the plain (no `--oem`) run.

**Files:**
- Modify: `tests/host/083_abl_patcher_oem.sh`

**Acceptance Criteria:**
- [ ] Coverage-note comment block at the top is updated — no longer mentions `GBL_PATCH7_ENABLED`
- [ ] Test 1 (`--oem oneplus --no-mode1`) additionally asserts `grep -qF 'patch7-orange-screen' "$OUT/m2.log"`
- [ ] Test 2 (plain, default) additionally asserts `! grep -qF 'patch7-orange-screen' "$OUT/m1.log"` (the OEM block is not aggregated when `--oem` defaults to `GBL_OEM_NONE`)
- [ ] Existing checks (mode_1 patch presence/absence, unknown-oem error) keep their existing assertions and pass

**Verify:**
```
bash tests/host/083_abl_patcher_oem.sh
```
Expected: `ok` lines for each check, no `FAIL`, exit 0. SKIP-guarded if fixture absent.

**Steps:**

- [ ] **Step 1: Update the coverage-note comment block**

Find the comment block (lines ~9-13) that begins `# Coverage note: this test exercises --oem *routing*...`. Replace it with:

```sh
# Coverage note: this test verifies both --oem *routing* (the scope-selection
# path through EnsureInitScoped) and OEM-patch *application* (patch7-orange-
# screen, the sole OEM patch as of 2026-05-20).
```

- [ ] **Step 2: Add patch7 presence check to Test 1**

In the "Test 1: --oem oneplus --no-mode1" block, after the two existing `grep -qF` mode_1 absence checks and before `echo "  ok: --oem oneplus --no-mode1 excludes mode_1 patches"`, add:

```sh
if ! grep -qF 'patch7-orange-screen' "$OUT/m2.log"; then
    echo "FAIL: oem patch7 absent from --oem oneplus run"
    cat "$OUT/m2.log"
    exit 1
fi
```

Then update the `echo` line to also reflect the new check:

```sh
echo "  ok: --oem oneplus --no-mode1 excludes mode_1 patches, includes oem patch7"
```

- [ ] **Step 3: Add patch7 absence check to Test 2**

In the "Test 2: default (mode-1) invocation" block, after the two existing `grep -qF` mode_1 presence checks and before `echo "  ok: plain invocation includes mode_1 patches"`, add:

```sh
if grep -qF 'patch7-orange-screen' "$OUT/m1.log"; then
    echo "FAIL: oem patch7 present in default (mode-1) run"
    cat "$OUT/m1.log"
    exit 1
fi
```

Update the `echo` line:

```sh
echo "  ok: plain invocation includes mode_1 patches, excludes oem patch7"
```

- [ ] **Step 4: Run the test**

```bash
bash tests/host/083_abl_patcher_oem.sh
```
Expected (fixture present): both `ok:` lines plus the existing regression-gate sibling tests pass. If the fixture is absent the script prints `SKIP: tests/images/pe/infiniti-EU-16.0.5.703.efi missing ...` and exits 0.

- [ ] **Step 5: Run the regression gate sibling tests directly to make sure nothing slipped**

```bash
bash tests/host/060_pack_roundtrip.sh
bash tests/host/062_efisp_scan_gate.sh
bash tests/045_mode_taxonomy_lint.sh
```
Expected: all `ok` or `SKIP`, none `FAIL`.

- [ ] **Step 6: Commit**

```bash
git add tests/host/083_abl_patcher_oem.sh
git commit -m "$(cat <<'EOF'
test(host): 083 asserts patch7 applied with --oem oneplus

Closes the TODO documented in the original 083 coverage note: now that
patch7 is unarchived, this test verifies both routing AND application.
patch7-orange-screen must appear in the --oem oneplus run's per-patch
log lines, and must NOT appear in the plain (no --oem) run.
EOF
)"
```

```json:metadata
{"files": ["tests/host/083_abl_patcher_oem.sh"], "verifyCommand": "bash tests/host/083_abl_patcher_oem.sh", "acceptanceCriteria": ["Coverage note updated (no GBL_PATCH7_ENABLED reference)", "Test 1 asserts patch7-orange-screen present in --oem oneplus run", "Test 2 asserts patch7-orange-screen absent in default run", "Regression gate (060, 062, 045) still passes"]}
```

---

## Task 4: Widen `zip/modes/mode-2-install.sh` OEM detect

**Goal:** `detect_oem` accepts the full oplus family — oneplus, oppo, oplus, realme — all mapped to `OEM_ID=oneplus`.

**Files:**
- Modify: `zip/modes/mode-2-install.sh`

**Acceptance Criteria:**
- [ ] Case branch at line 40 reads `*oneplus*|*oppo*|*oplus*|*realme*) OEM_ID=oneplus ;;`
- [ ] Unknown OEM still aborts with the existing message
- [ ] `bash -n zip/modes/mode-2-install.sh` parses cleanly
- [ ] Existing zip tests (if any cover mode-2 install) still pass

**Verify:**
```
bash -n zip/modes/mode-2-install.sh
grep -n 'oneplus.*oppo.*oplus.*realme' zip/modes/mode-2-install.sh
```
Expected: parse succeeds (exit 0); grep finds exactly one line matching the case pattern.

**Steps:**

- [ ] **Step 1: Edit the case branch**

In `zip/modes/mode-2-install.sh`, locate `detect_oem()` (currently starting around line 31). Change:

```sh
  case "$_mfr" in
    *oneplus*|*oppo*) OEM_ID=oneplus ;;
    *) abort "unsupported OEM (build.prop manufacturer='$_mfr')" ;;
  esac
```

to:

```sh
  case "$_mfr" in
    *oneplus*|*oppo*|*oplus*|*realme*) OEM_ID=oneplus ;;
    *) abort "unsupported OEM (build.prop manufacturer='$_mfr')" ;;
  esac
```

- [ ] **Step 2: Syntax-check and confirm the change**

```bash
bash -n zip/modes/mode-2-install.sh
grep -n 'oneplus.*oppo.*oplus.*realme' zip/modes/mode-2-install.sh
```
Expected: `bash -n` exits 0; `grep` prints the single matching line.

- [ ] **Step 3: Run any zip-related host tests, if present**

```bash
for t in tests/host/0*_install_*.sh tests/host/0*_mode2_*.sh; do
  [ -f "$t" ] || continue
  echo "== $t =="
  bash "$t"
done
```
Expected: each test prints `PASS` / `ok` / `SKIP`; none `FAIL`. If none of these scripts exercise `detect_oem` directly, that's fine — the change is shell-script-only.

- [ ] **Step 4: Commit**

```bash
git add zip/modes/mode-2-install.sh
git commit -m "$(cat <<'EOF'
feat(mode-2-zip): detect_oem accepts oplus/realme

Widen the OEM-detect case branch to *oneplus*|*oppo*|*oplus*|*realme*.
Same OEM_ID (oneplus) — same oplus-family ABL fork. patch7's anchor on
Realme/Oppo ABLs is unverified; PATCH_MISS is a clean no-op so failure
is safe.
EOF
)"
```

```json:metadata
{"files": ["zip/modes/mode-2-install.sh"], "verifyCommand": "bash -n zip/modes/mode-2-install.sh && grep -n 'oneplus.*oppo.*oplus.*realme' zip/modes/mode-2-install.sh", "acceptanceCriteria": ["case branch matches *oneplus*|*oppo*|*oplus*|*realme*", "abort path unchanged", "bash -n parses clean"]}
```

---

## Task 5: On-device verification — orange-state warning silenced in mode-2

**Goal:** **USER-ORDERED GATE — NON-SKIPPABLE.** This task was requested by the user in the current conversation. It MUST NOT be closed by walking around it, by declaring it "verified inline", or by substituting a cheaper check. Close only after every item in `acceptanceCriteria` has been re-validated independently, with output captured.

Build a mode-2 EFI with patch7 enabled, stage it on the infiniti test phone (never `fastboot flash`), and confirm:
1. The orange-state warning screen no longer renders on boot.
2. The mode-2 attestation baseline (key attestation, Widevine, RKP, Strongbox, SOTER — all passing as of 2026-05-18) still passes.

**Files:** none (host build + on-device execution + log capture).

**Acceptance Criteria:**
- [ ] mode-2 EFI built locally (`scripts/build.sh --mode 2`), producing `dist/mode-2.efi`
- [ ] EFI staged via `fastboot stage dist/mode-2.efi` followed by `fastboot oem boot-efi` — no `fastboot flash` for any partition
- [ ] On boot, the orange-state warning screen does NOT render (visual confirmation — record on phone camera if helpful)
- [ ] `dmesg` and `logcat` captured after boot to `logs/<YYYYMMDD-HHMMSS>_manual_mode2-patch7-on_<short-sha>/`
- [ ] Key attestation app reports PASS (whatever the established harness reports — same as 2026-05-18 baseline)
- [ ] Widevine L1 / RKP / Strongbox / SOTER status unchanged from the 2026-05-18 baseline
- [ ] Captured log directory referenced in the eventual PR description

**Verify:**

Visual + log-based, on the test phone:

```bash
scripts/build.sh --mode 2
# Build prints "dist/mode-2.efi" path on success.

fastboot devices
# Expected: the infiniti device appears.

fastboot stage dist/mode-2.efi
fastboot oem boot-efi
# Expected: device boots into Android. Orange-state warning screen NOT shown.

# Capture logs into the conventional logs/ path
ts=$(date +%Y%m%d-%H%M%S)
sha=$(git rev-parse --short HEAD)
out="logs/${ts}_manual_mode2-patch7-on_v${sha}"
mkdir -p "$out"
adb wait-for-device
adb shell dmesg                > "$out/dmesg.txt"
adb logcat -d                  > "$out/logcat.txt"
adb shell getprop              > "$out/getprop.txt"
```

Then on-device, manually re-run the attestation suite (key attestation app, Widevine info, RKP status, Strongbox, SOTER) and add screenshots / output to `${out}/`.

**Steps:**

- [ ] **Step 1: Confirm working tree is clean and on the right branch**

```bash
git status
git rev-parse --abbrev-ref HEAD   # feat/patch7-mode2-enable
git log --oneline main..HEAD
# expected: spec commit + Task 1 + Task 2 + Task 3 + Task 4 commits = 5 commits
```

- [ ] **Step 2: Build the mode-2 EFI**

```bash
scripts/build.sh --mode 2
ls -la dist/mode-2.efi
```

Expected: the build script succeeds and writes `dist/mode-2.efi`. Capture full build log:

```bash
scripts/build.sh --mode 2 2>&1 | tee "/tmp/patch7-mode2-build.log"
```

- [ ] **Step 3: Confirm the test phone is in fastboot**

```bash
fastboot devices
```

Expected: at least one device listed. If empty, reboot the phone into bootloader (e.g. `adb reboot bootloader`) and try again.

- [ ] **Step 4: Stage and boot the EFI**

⚠️ **SAFETY GATE (CLAUDE.md §Safety):** Do NOT issue `fastboot flash`, `fastboot oem unlock/lock`, `fastboot flashing unlock_critical`, `fastboot --set-active`, or `fastboot erase` for any non-HLOS partition. The PreToolUse hook will block these regardless. The ONLY allowed sequence is `stage` + `oem boot-efi`.

```bash
fastboot stage dist/mode-2.efi
fastboot oem boot-efi
```

Expected: the chained EFI runs, ABL hands off, Android boots.

- [ ] **Step 5: Visual confirmation — orange-state warning is gone**

Watch the screen during boot. The orange-state warning block (the 5-second unlock-warning delay) should NOT render. If a camera capture is needed, take a short video on a second phone.

- [ ] **Step 6: Capture logs**

```bash
ts=$(date +%Y%m%d-%H%M%S)
sha=$(git rev-parse --short HEAD)
out="logs/${ts}_manual_mode2-patch7-on_v${sha}"
mkdir -p "$out"
adb wait-for-device
adb shell dmesg                > "$out/dmesg.txt"
adb logcat -d                  > "$out/logcat.txt"
adb shell getprop              > "$out/getprop.txt"
cp /tmp/patch7-mode2-build.log "$out/build.log" 2>/dev/null || true
echo "patch7 mode-2 on-device verification, branch feat/patch7-mode2-enable HEAD=${sha}" > "$out/README.txt"
```

- [ ] **Step 7: Re-run the mode-2 attestation baseline**

Following the same flow as the 2026-05-18 baseline (memory: `mode2_validated_on_device.md`):
  - Run the key-attestation app on the phone; record verdict + any anomaly screenshots into `${out}/`.
  - Capture Widevine info (`adb shell getprop | grep -i widevine`, plus the relevant app status).
  - Check RKP status (`adb shell cmd remote_provisioning -- status` or whatever your harness uses; same as 2026-05-18).
  - Strongbox status — same source as baseline.
  - SOTER status — same source as baseline.

Save all outputs into `${out}/` as separate files (`keyattest.txt`, `widevine.txt`, `rkp.txt`, etc.).

- [ ] **Step 8: Compare against baseline**

Each of the 5 attestation surfaces must report the same verdict as the 2026-05-18 baseline (all PASS). Diff any deltas; if a regression is observed, capture details and DO NOT close the task.

- [ ] **Step 9: Commit the log directory pointer (optional — logs/ is typically gitignored)**

If `logs/` is gitignored (check `.gitignore`): no commit needed for the log directory. Just keep the path reference for the PR description.

If `logs/` is tracked: `git add "$out" && git commit -m "logs: mode-2 patch7 on-device verification"`.

```bash
# check status
grep -E '^logs?/' .gitignore 2>/dev/null
```

- [ ] **Step 10: Mark complete only after all acceptance criteria met**

Re-read the **Acceptance Criteria** list above. Each line must have an observed-output backing it. If any item is missing observation, the task stays open.

```json:metadata
{"files": [], "verifyCommand": "scripts/build.sh --mode 2 && echo MANUAL_STEPS_REQUIRED", "acceptanceCriteria": ["dist/mode-2.efi built from feat/patch7-mode2-enable HEAD", "EFI staged via fastboot stage + oem boot-efi (no flash)", "orange-state warning screen does NOT render on boot (visual)", "dmesg + logcat + getprop captured under logs/<ts>_manual_mode2-patch7-on_v<sha>/", "key attestation reports PASS (matches 2026-05-18 baseline)", "Widevine/RKP/Strongbox/SOTER unchanged from 2026-05-18 baseline"], "userGate": true, "tags": ["user-gate"], "gateScope": "user", "failurePolicy": "stop"}
```

---

## Final integration: PR

Once Task 5 is closed:

- [ ] Push `feat/patch7-mode2-enable` to origin: `git push -u origin feat/patch7-mode2-enable`
- [ ] Open a PR against `main` summarizing:
  - Goal (one-liner from the spec).
  - Files changed (`oneplus_canoe.c`, `test_patch7.c`, `083_abl_patcher_oem.sh`, `mode-2-install.sh`).
  - Reference the spec doc and the on-device verification log directory.
- [ ] Link the PR back to the spec commit and this plan.
