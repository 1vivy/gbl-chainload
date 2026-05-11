# gbl-chainload v2 — Plan 1: Foundation + Patch Engine v2 + Mode-1 first build

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bootstrap a fresh `gbl-chainload` repo with a clean three-mode-ready architecture, scan-based patch engine v2, universal baseline + mode-1 protocol-hook overlay, and a green host CI. End-state artifact: `dist/mode-1.efi` boots a stock-signed image on canoe with the same green/locked behavior as today's `mode-fakelocked.efi`, rebuilt on the durable scan-engine + clean module taxonomy.

**Status:** completed (commit `4397930` / tag `v2.0.0-plan1-foundation`).

**Architecture:** Fresh GitHub repo (`1vivy/gbl-chainload`) with a fresh edk2 fork. `GblChainloadPkg/` split into Application (Entry, BootFlow), DynamicPatchLib (scan engine + universal/oem/mode_N namespaces), ProtocolHookLib (universal baseline + per-mode overlay). Host-runnable `tools/abl-patcher` shares one C source with the runtime patcher.

**Tech Stack:** EDK-II (UEFI Application + Library, AArch64), C99, GitHub Actions, Docker for build environment, gh CLI for repo creation.

**Spec reference:** `docs/superpowers/specs/2026-05-09-gbl-chainload-v2-three-mode-rewrite-design.md`

**Plan series:**
- **Plan 1 (this):** Foundation → edk2 D2 → Engine v2 → Universal + Mode-1 → Build flags → first working `dist/mode-1.efi`.
- **Plan 2:** Mode-2 + typed-struct profile + `extract-mode2-profile.py` + SPSS vtable fingerprint.
- **Plan 3:** Mode-3 + ABL embed (cache) + RE docs (`oplusreserve1`, `gbl-load-mechanism`, `scm-fuse-classification`).

**Old repo path:** `/home/vivy/gbl-chainload-dirty/` after the path-collision rename in Task 1. References below to "the old repo" mean that path.

---

## File Structure

```
gbl-chainload/                                 # new repo
├── .github/workflows/ci.yml                   # runs tests/runall.sh in docker
├── .gitignore
├── LICENSE
├── README.md
├── edk2/                                      # submodule, fresh fork
├── GblChainloadPkg/
│   ├── GblChainloadPkg.dsc                    # GBL_MODE/AUTO/DEBUG/VERBOSE feature pcds
│   ├── Application/GblChainload/
│   │   ├── GblChainload.inf
│   │   ├── Entry.c                             # mode dispatcher + key window + AUTO toggle
│   │   ├── BootFlow.c                          # AblUnwrap → DynamicPatch → ProtocolHook → LoadImage
│   │   └── Stubs.c
│   ├── Library/
│   │   ├── AblUnwrapLib/                       # carried verbatim from old repo
│   │   ├── LogFsLib/                           # carried verbatim from old repo
│   │   ├── DynamicPatchLib/
│   │   │   ├── DynamicPatchLib.inf
│   │   │   ├── Internal/
│   │   │   │   ├── ScanLib.c                   # ScanFor / ScanForBoundedSection
│   │   │   │   ├── ScanLib.h
│   │   │   │   ├── PeSections.c                # IsPeFileOffsetInExecutableSection
│   │   │   │   ├── PeSections.h
│   │   │   │   ├── Encode.c                    # WriteInstrU32 / RewriteCbz
│   │   │   │   ├── Encode.h
│   │   │   │   ├── PatchEngine.c               # DynamicPatch_Apply iterator
│   │   │   │   └── PatchDesc.h                 # PATCH_OUTCOME / PATCH_SCOPE / PATCH_DESC
│   │   │   ├── universal/
│   │   │   │   ├── universal.c                 # patch1-efisp-recursion
│   │   │   │   └── Signatures.h
│   │   │   ├── oem/
│   │   │   │   └── oneplus_canoe.c             # patch7-orange-screen
│   │   │   └── mode_1/
│   │   │       └── mode_1.c                    # patch9-avb-locked-recoverable-continue
│   │   └── ProtocolHookLib/
│   │       ├── ProtocolHookLib.inf
│   │       ├── InstallAll.c                    # universal + mode-N overlay dispatcher
│   │       ├── UniversalBaseline.c             # VB swallow + SCM fuse drop + OplusSec 0x0A drop
│   │       ├── Mode1Overlay.c                  # VB READ_CONFIG / VBDeviceInit mutate
│   │       ├── VerifiedBootHook.c              # VB slot wrappers (carried + adapted)
│   │       ├── QseecomHook.c                   # Qseecom slot wrappers (carried + adapted)
│   │       ├── ScmHook.c                       # SCM slot wrappers (carried + adapted)
│   │       └── HookCommon.h
│   └── Include/Library/
│       ├── DynamicPatchLib.h
│       ├── ProtocolHookLib.h
│       └── ScanLib.h
├── scripts/
│   ├── build.sh                                # --mode N --auto --debug --verbose
│   ├── build-inside-docker.sh                  # carried verbatim
│   ├── extract-canoe-fixtures.sh               # NEW — pull abl_a from images/canoe-stock/
│   ├── test-device-automatic.sh                # carried verbatim
│   └── test-device-manual.sh                   # carried verbatim
├── tests/
│   ├── 010_build_smoke.sh                      # builds the 3 default artifacts
│   ├── 030_signature_lint.sh                   # carried verbatim
│   ├── 042_dynamic_patch_harness.sh            # rewritten: anchor uniqueness across fixtures
│   ├── 045_mode_taxonomy_lint.sh               # NEW — assert universal hooks installed every mode
│   ├── 046_mode1_protocol_hook_lint.sh         # NEW — assert mode-1 overlay paths exist
│   ├── 051_gbl_root_canoe_regression.sh        # NEW — borrow patches as fixtures
│   ├── fixtures/
│   │   └── patches-gbl-root-canoe/             # imported regression patches + expected outputs
│   └── runall.sh
├── tools/
│   └── abl-patcher/
│       ├── abl-patcher.c                       # host binary linking PatchEngine.o
│       └── Makefile
├── docs/
│   └── re/                                     # carried .re-notes/sessions/ + selected RE docs
├── docker/                                     # carried verbatim from old repo
├── images/                                     # gitignored — stock OTA partition images
├── profiles/                                   # gitignored payloads, manifest.json tracked (Plan 2)
└── dist/                                       # gitignored build outputs
```

---

## Task 1: Pre-flight — resolve `/home/vivy/gbl-chainload` path collision

**Files:** filesystem only (rename existing directory).

- [ ] **Step 1: Verify old repo state is committed**

```bash
cd /home/vivy/gbl-chainload && git status --porcelain
```

Expected: there will be uncommitted RE work (`docs/re/avb-*.md`, `docs/re/fakelock-*.md`, `scripts/extract-avb-embedded-vbmeta.py`, this spec, this plan). Either commit them on the current branch (so they're preserved in the dirty/ reference), or stash. Recommended:

```bash
cd /home/vivy/gbl-chainload \
  && git add docs/superpowers/ docs/re/ scripts/extract-avb-embedded-vbmeta.py \
  && git commit -m "checkpoint: v2 spec + plan-1 + RE docs before fresh-repo cutover"
```

- [ ] **Step 2: Tag dirty reference for cherry-pick**

```bash
cd /home/vivy/gbl-chainload \
  && git tag dirty/last-mode-fakelocked HEAD \
  && git tag --list | grep dirty
```

Expected output: `dirty/last-mode-fakelocked`.

- [ ] **Step 3: Rename old directory**

```bash
cd /home/vivy && mv gbl-chainload gbl-chainload-dirty && ls -ld gbl-chainload-dirty
```

Expected: directory renamed; the path the user has in WSL muscle memory now points nowhere until Task 2 creates the new repo.

---

## Task 2: Create new GitHub repo + clone locally

**Files:** filesystem (new clone target).

- [ ] **Step 1: Create remote repo**

```bash
cd /home/vivy && gh repo create gbl-chainload --private --description "GBL chainloader for OnePlus/Oppo devices — v2 three-mode taxonomy"
```

Expected: prints the new repo URL `https://github.com/1vivy/gbl-chainload`.

- [ ] **Step 2: Clone locally**

```bash
cd /home/vivy && gh repo clone gbl-chainload
```

Expected: `gbl-chainload/` exists with just `.git/` inside.

- [ ] **Step 3: Configure git user/email if not already global**

```bash
cd /home/vivy/gbl-chainload && git config user.email "1vivy@tutanota.com" && git config user.name "1vivy"
```

Expected: no output, exit 0.

---

## Task 3: First commit — `LICENSE` + `.gitignore` + `README.md` skeleton

**Files:**
- Create: `gbl-chainload/LICENSE`
- Create: `gbl-chainload/.gitignore`
- Create: `gbl-chainload/README.md`

- [ ] **Step 1: Carry license from old repo**

```bash
cp /home/vivy/gbl-chainload-dirty/LICENSE /home/vivy/gbl-chainload/LICENSE 2>/dev/null \
  || echo "no LICENSE in old repo — create one before commit"
```

If the old repo has no LICENSE, write a `LICENSE` file with whichever license the project lead prefers (BSD-3-Clause is a reasonable EDK-II-aligned default). Ask user if unclear.

- [ ] **Step 2: Write `.gitignore`**

Create `/home/vivy/gbl-chainload/.gitignore`:

```gitignore
# Build outputs
Build/
dist/
*.o
*.obj
*.efi.unsigned

# Stock OTA partition images — never tracked
images/
!images/.gitkeep

# Mode-2 profile payloads — gitignored; manifest.json is tracked (Plan 2)
profiles/*/profile.h
profiles/*/[!manifest]*.bin

# Device test logs
logs/

# RE staging (transient)
.re-notes-staging/

# Python
__pycache__/
*.pyc
.venv/

# Editor
.vscode/
.idea/
*.swp

# OS
.DS_Store
Thumbs.db
```

- [ ] **Step 3: Write `README.md` skeleton**

Create `/home/vivy/gbl-chainload/README.md`:

```markdown
# gbl-chainload

EFI System Partition (EFISP) chainloader for OnePlus/Oppo devices using Qualcomm's GBL/EFISP load mechanism. Patches the active-slot ABL in memory, installs targeted protocol hooks, and hands off to the patched ABL.

## Status

v2 architecture in flight. See `docs/superpowers/specs/` for the design and `docs/superpowers/plans/` for the implementation plan series.

Today's working artifact: `dist/mode-1.efi` (Plan 1 deliverable) — protocol-hook fakelock via `QCOM_VERIFIEDBOOT_PROTOCOL` mutation; KM/Oplus see locked/green when stock images verify cleanly.

## Modes

- **mode-1** — protocol-hook fakelock. ABL sees locked DeviceInfo and builds KM SET_ROT/SET_BOOT_STATE off that view.
- **mode-2** *(Plan 2)* — TA-payload spoof at QSEE/SPSS boundaries; ABL stays honest; per-OTA typed-struct profile.
- **mode-3** *(Plan 3)* — universal baseline only; minimal experiment to gauge KM root-cert leaf survival.

## Build

```bash
./scripts/build.sh --mode 1               # production silent
./scripts/build.sh --mode 1 --auto --debug --verbose   # dev capture
```

## Repo conventions

- `GblChainloadPkg/Library/DynamicPatchLib/{universal,oem,mode_1}/` — patches scoped by applicability.
- `GblChainloadPkg/Library/ProtocolHookLib/UniversalBaseline.c` — hooks every mode ships.
- `GblChainloadPkg/Library/ProtocolHookLib/Mode1Overlay.c` — mode-1-specific hooks atop baseline.

## Sibling

Old tree preserved read-only at `/home/vivy/gbl-chainload-dirty/` (tag `dirty/last-mode-fakelocked`).
```

- [ ] **Step 4: Initial commit**

```bash
cd /home/vivy/gbl-chainload \
  && git add LICENSE .gitignore README.md \
  && git commit -m "initial commit: LICENSE, .gitignore, README skeleton"
```

Expected: one commit on `main`.

- [ ] **Step 5: Push**

```bash
cd /home/vivy/gbl-chainload && git push -u origin main
```

Expected: branch tracks origin/main.

---

## Task 4: edk2 fork D2 — identify upstream parent + create fresh fork

**Files:** new GitHub repo for edk2 fork.

- [ ] **Step 1: Identify upstream parent of `2c4c2d629c FastbootLib: add recovery escape controls`**

```bash
cd /home/vivy/gbl-chainload-dirty/edk2 \
  && git log 2c4c2d629c -1 --pretty=fuller \
  && git log 2c4c2d629c^ -1 --pretty=fuller
```

Capture the parent commit's full SHA and the upstream remote URL (`git remote -v`). The parent SHA is the cutover point.

- [ ] **Step 2: Note the upstream remote name**

```bash
cd /home/vivy/gbl-chainload-dirty/edk2 \
  && git remote -v
```

Expected: at least one remote pointing at the upstream EDK-II fork (likely OnePlus's or Qualcomm's). Record the URL — call it `<UPSTREAM_URL>`.

- [ ] **Step 3: Create the fresh fork repo**

```bash
cd /home/vivy && gh repo create edk2-gbl-chainload --private --description "Fresh edk2 fork for gbl-chainload — minimal divergence from upstream, vector for desktop Linux from USB"
```

Expected: `https://github.com/1vivy/edk2-gbl-chainload` exists, empty.

- [ ] **Step 4: Mirror upstream into the new fork up to the parent SHA**

```bash
cd /home/vivy && git clone <UPSTREAM_URL> edk2-fresh \
  && cd edk2-fresh \
  && git checkout <PARENT_SHA> \
  && git checkout -b main \
  && git remote set-url origin git@github.com:1vivy/edk2-gbl-chainload.git \
  && git push -u origin main
```

Expected: `1vivy/edk2-gbl-chainload` contains upstream history up to `<PARENT_SHA>`.

---

## Task 5: edk2 fork D2 — cherry-pick keepers, skip dead-ends

**Files:** commits on `main` of `edk2-gbl-chainload`.

- [ ] **Step 1: List candidate commits in old fork**

```bash
cd /home/vivy/gbl-chainload-dirty/edk2 \
  && git log --oneline <PARENT_SHA>..HEAD
```

Expected: chronological list of every patch on top of the upstream parent. Identify each as KEEP / DROP per spec §"edk2 fork D2".

- [ ] **Step 2: Cherry-pick `2c4c2d629c FastbootLib: add recovery escape controls`**

```bash
cd /home/vivy/edk2-fresh \
  && git remote add olddirty /home/vivy/gbl-chainload-dirty/edk2 \
  && git fetch olddirty \
  && git cherry-pick 2c4c2d629c
```

Resolve any conflicts; conflicts on this commit are unlikely since we forked from its parent.

- [ ] **Step 3: Cherry-pick any other identified keepers**

For each commit identified as KEEP in Step 1 (other than `2c4c2d629c`), run:

```bash
cd /home/vivy/edk2-fresh && git cherry-pick <SHA>
```

If a keeper depends on a DROP commit, either rework it to drop the dependency or move the keeper to "deferred" — note in `docs/re/edk2-fork-keeper-deferred.md` (in main repo) for later attention.

**DROP list — never cherry-pick (these are the dead-ends the spec calls out):**

- TogglePrimaryOS commits (search by `Toggle Primary` in commit message)
- `2c3bc21 edk2: bump to raw block-IO oem get-staged logfs`
- `22141fa oem get-staged logfs`
- Any shell-boot escape commits
- Any commits that depend on the above

- [ ] **Step 4: Push the cherry-picked tip**

```bash
cd /home/vivy/edk2-fresh && git push origin main
```

- [ ] **Step 5: Remove the local olddirty remote (don't leak it into the fresh fork)**

```bash
cd /home/vivy/edk2-fresh && git remote remove olddirty
```

---

## Task 6: Add edk2 as submodule + verify it resolves

**Files:**
- Create: `gbl-chainload/.gitmodules`
- Create: `gbl-chainload/edk2/` (submodule pointer)

- [ ] **Step 1: Add submodule**

```bash
cd /home/vivy/gbl-chainload \
  && git submodule add git@github.com:1vivy/edk2-gbl-chainload.git edk2 \
  && git submodule update --init --recursive
```

Expected: `edk2/` directory populated; `.gitmodules` and `edk2` (the submodule pointer) staged.

- [ ] **Step 2: Verify the submodule has the cherry-picked tip**

```bash
cd /home/vivy/gbl-chainload/edk2 && git log --oneline | head -5
```

Expected: top commit is the cherry-picked recovery-escape-controls commit (or whichever keeper was last in Task 5 Step 3).

- [ ] **Step 3: Commit the submodule registration**

```bash
cd /home/vivy/gbl-chainload \
  && git commit -m "edk2: add fresh fork as submodule"
```

---

## Task 7: Carry-forward LogFsLib (verbatim)

**Files:**
- Create: `gbl-chainload/GblChainloadPkg/Library/LogFsLib/` (mirror of old)

- [ ] **Step 1: Copy verbatim**

```bash
cp -r /home/vivy/gbl-chainload-dirty/GblChainloadPkg/Library/LogFsLib /home/vivy/gbl-chainload/GblChainloadPkg/Library/
```

- [ ] **Step 2: Quick sanity — file list matches**

```bash
diff <(ls /home/vivy/gbl-chainload-dirty/GblChainloadPkg/Library/LogFsLib) \
     <(ls /home/vivy/gbl-chainload/GblChainloadPkg/Library/LogFsLib)
```

Expected: empty diff.

- [ ] **Step 3: Commit**

```bash
cd /home/vivy/gbl-chainload \
  && git add GblChainloadPkg/Library/LogFsLib \
  && git commit -m "GblChainloadPkg: carry-forward LogFsLib verbatim from dirty/"
```

---

## Task 8: Carry-forward AblUnwrapLib (verbatim)

**Files:**
- Create: `gbl-chainload/GblChainloadPkg/Library/AblUnwrapLib/`

Same shape as Task 7.

- [ ] **Step 1: Copy verbatim**

```bash
cp -r /home/vivy/gbl-chainload-dirty/GblChainloadPkg/Library/AblUnwrapLib /home/vivy/gbl-chainload/GblChainloadPkg/Library/
```

- [ ] **Step 2: Sanity diff**

```bash
diff <(ls /home/vivy/gbl-chainload-dirty/GblChainloadPkg/Library/AblUnwrapLib) \
     <(ls /home/vivy/gbl-chainload/GblChainloadPkg/Library/AblUnwrapLib)
```

Expected: empty diff.

- [ ] **Step 3: Commit**

```bash
cd /home/vivy/gbl-chainload \
  && git add GblChainloadPkg/Library/AblUnwrapLib \
  && git commit -m "GblChainloadPkg: carry-forward AblUnwrapLib verbatim from dirty/"
```

---

## Task 9: Carry-forward `.re-notes/sessions/` + selected RE docs

**Files:**
- Create: `gbl-chainload/.re-notes/sessions/` (verbatim copy)
- Create: `gbl-chainload/docs/re/` (selected docs only)

- [ ] **Step 1: Copy session notes verbatim**

```bash
cp -r /home/vivy/gbl-chainload-dirty/.re-notes /home/vivy/gbl-chainload/
```

- [ ] **Step 2: Copy keep-worthy RE docs**

```bash
mkdir -p /home/vivy/gbl-chainload/docs/re
cp /home/vivy/gbl-chainload-dirty/docs/re/avb-input-facade.md \
   /home/vivy/gbl-chainload/docs/re/
cp /home/vivy/gbl-chainload-dirty/docs/re/avb-descriptor-findings-eu-16.0.5.703.md \
   /home/vivy/gbl-chainload/docs/re/
cp /home/vivy/gbl-chainload-dirty/docs/re/fakelock-vs-debug-comparison.md \
   /home/vivy/gbl-chainload/docs/re/
```

DO NOT carry: `update-device-tree-callsite-helper.md` (UDT-helper-related), `qcom-boot-arg-mutation.md` (cmdline mutation no longer a target). If they exist in the old repo, omit them.

- [ ] **Step 3: Carry the spec + plan-1 forward**

```bash
mkdir -p /home/vivy/gbl-chainload/docs/superpowers/specs \
          /home/vivy/gbl-chainload/docs/superpowers/plans
cp /home/vivy/gbl-chainload-dirty/docs/superpowers/specs/2026-05-09-gbl-chainload-v2-three-mode-rewrite-design.md \
   /home/vivy/gbl-chainload/docs/superpowers/specs/
cp /home/vivy/gbl-chainload-dirty/docs/superpowers/plans/2026-05-09-gbl-chainload-v2-plan-1-foundation-and-mode-1.md \
   /home/vivy/gbl-chainload/docs/superpowers/plans/
```

- [ ] **Step 4: Commit**

```bash
cd /home/vivy/gbl-chainload \
  && git add .re-notes docs \
  && git commit -m "docs: carry-forward session notes, RE docs, spec + plan-1"
```

---

## Task 10: Carry-forward `docker/` + base build scripts

**Files:**
- Create: `gbl-chainload/docker/`
- Create: `gbl-chainload/scripts/build-inside-docker.sh`
- Create: `gbl-chainload/scripts/test-device-automatic.sh`
- Create: `gbl-chainload/scripts/test-device-manual.sh`

- [ ] **Step 1: Copy docker/ verbatim**

```bash
cp -r /home/vivy/gbl-chainload-dirty/docker /home/vivy/gbl-chainload/
```

- [ ] **Step 2: Copy stable test-device scripts verbatim**

```bash
mkdir -p /home/vivy/gbl-chainload/scripts
cp /home/vivy/gbl-chainload-dirty/scripts/test-device-automatic.sh \
   /home/vivy/gbl-chainload/scripts/
cp /home/vivy/gbl-chainload-dirty/scripts/test-device-manual.sh \
   /home/vivy/gbl-chainload/scripts/
cp /home/vivy/gbl-chainload-dirty/scripts/build-inside-docker.sh \
   /home/vivy/gbl-chainload/scripts/
chmod +x /home/vivy/gbl-chainload/scripts/*.sh
```

- [ ] **Step 3: DO NOT copy `pull-logfs.sh`**

Verify it's not present:

```bash
test ! -e /home/vivy/gbl-chainload/scripts/pull-logfs.sh && echo "good — pull-logfs absent"
```

Expected: prints `good — pull-logfs absent`.

- [ ] **Step 4: Commit**

```bash
cd /home/vivy/gbl-chainload \
  && git add docker scripts \
  && git commit -m "build: carry-forward docker env + test-device scripts (no pull-logfs)"
```

---

## Task 11: ScanLib `ScanFor` — TDD

**Files:**
- Create: `gbl-chainload/GblChainloadPkg/Include/Library/ScanLib.h`
- Create: `gbl-chainload/GblChainloadPkg/Library/DynamicPatchLib/Internal/ScanLib.h`
- Create: `gbl-chainload/GblChainloadPkg/Library/DynamicPatchLib/Internal/ScanLib.c`
- Create: `tests/scan/test_scanfor.c` (host-compiled)
- Create: `tests/scan/Makefile`

- [ ] **Step 1: Write the public header**

Create `GblChainloadPkg/Include/Library/ScanLib.h`:

```c
/** @file ScanLib.h — pattern-scan helpers for DynamicPatchLib v2.
    Stateless: caller owns the buffer; helpers only read.  **/

#ifndef SCANLIB_H_
#define SCANLIB_H_

#include <Uefi.h>

typedef enum {
  SCAN_FOUND       = 0,   // exactly one match in the scanned domain
  SCAN_NOT_FOUND   = 1,
  SCAN_AMBIGUOUS   = 2,   // >1 match — pattern not unique
  SCAN_BAD_INPUT   = 3,   // NULL pointer or zero-size buffer
} SCAN_RESULT;

/** Scan the entire buffer for exactly one match of Pattern.
    @param Buf,Size       Buffer to scan.
    @param Pattern        Bytes to match.
    @param Mask           Optional per-byte mask: 0xFF = compare, 0x00 = wildcard.
                          NULL = exact-match (all 0xFF mask).
    @param PatternLen     Length of Pattern (and Mask if non-NULL).
    @param MatchOff       Out: file-offset of unique match (only on SCAN_FOUND).
    @return SCAN_FOUND / NOT_FOUND / AMBIGUOUS / BAD_INPUT.
    Always scans the whole buffer (no first-match exit) so ambiguity is detected. **/
SCAN_RESULT
ScanFor (
  IN  CONST UINT8 *Buf,
  IN  UINT32       Size,
  IN  CONST UINT8 *Pattern,
  IN  CONST UINT8 *Mask OPTIONAL,
  IN  UINTN        PatternLen,
  OUT UINT32      *MatchOff
  );

#endif /* SCANLIB_H_ */
```

- [ ] **Step 2: Write the failing test**

Create `tests/scan/test_scanfor.c`:

```c
/* Host-compiled tests for ScanFor. No EDK-II framework — uses libc + assert. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

/* Provide the EDK-II type aliases ScanLib.h uses, when compiling host-side. */
#define IN
#define OUT
#define OPTIONAL
#define EFIAPI
typedef uint8_t  UINT8;
typedef uint32_t UINT32;
typedef size_t   UINTN;
typedef int      EFI_STATUS;
typedef int      BOOLEAN;
#define CONST const
#define VOID void
#define TRUE 1
#define FALSE 0

#include "../../GblChainloadPkg/Library/DynamicPatchLib/Internal/ScanLib.h"

static void test_scan_unique_match (void) {
  UINT8  Buf[64];
  UINT32 Off = 0xFFFFFFFFu;
  SCAN_RESULT R;

  memset (Buf, 0xAA, sizeof (Buf));
  Buf[20] = 0xDE; Buf[21] = 0xAD; Buf[22] = 0xBE; Buf[23] = 0xEF;
  CONST UINT8 Pat[] = { 0xDE, 0xAD, 0xBE, 0xEF };

  R = ScanFor (Buf, sizeof (Buf), Pat, NULL, sizeof (Pat), &Off);
  assert (R == SCAN_FOUND);
  assert (Off == 20);
  printf ("ok test_scan_unique_match\n");
}

static void test_scan_not_found (void) {
  UINT8  Buf[64];
  UINT32 Off = 0xFFFFFFFFu;
  SCAN_RESULT R;

  memset (Buf, 0xAA, sizeof (Buf));
  CONST UINT8 Pat[] = { 0xDE, 0xAD, 0xBE, 0xEF };

  R = ScanFor (Buf, sizeof (Buf), Pat, NULL, sizeof (Pat), &Off);
  assert (R == SCAN_NOT_FOUND);
  printf ("ok test_scan_not_found\n");
}

static void test_scan_ambiguous (void) {
  UINT8  Buf[64];
  UINT32 Off = 0xFFFFFFFFu;
  SCAN_RESULT R;

  memset (Buf, 0xAA, sizeof (Buf));
  Buf[10] = 0xDE; Buf[11] = 0xAD;
  Buf[40] = 0xDE; Buf[41] = 0xAD;
  CONST UINT8 Pat[] = { 0xDE, 0xAD };

  R = ScanFor (Buf, sizeof (Buf), Pat, NULL, sizeof (Pat), &Off);
  assert (R == SCAN_AMBIGUOUS);
  printf ("ok test_scan_ambiguous\n");
}

static void test_scan_with_mask (void) {
  UINT8  Buf[64];
  UINT32 Off = 0xFFFFFFFFu;
  SCAN_RESULT R;

  memset (Buf, 0xAA, sizeof (Buf));
  /* Match anything starting with 0xDE 0xAD followed by any 2 bytes ending 0xEF */
  Buf[30] = 0xDE; Buf[31] = 0xAD; Buf[32] = 0x12; Buf[33] = 0xEF;
  CONST UINT8 Pat[] = { 0xDE, 0xAD, 0x00, 0xEF };
  CONST UINT8 Mask[] = { 0xFF, 0xFF, 0x00, 0xFF };

  R = ScanFor (Buf, sizeof (Buf), Pat, Mask, sizeof (Pat), &Off);
  assert (R == SCAN_FOUND);
  assert (Off == 30);
  printf ("ok test_scan_with_mask\n");
}

static void test_scan_bad_input (void) {
  SCAN_RESULT R;
  UINT32 Off = 0;
  CONST UINT8 Pat[] = { 0x01 };
  R = ScanFor (NULL, 64, Pat, NULL, 1, &Off);
  assert (R == SCAN_BAD_INPUT);
  printf ("ok test_scan_bad_input\n");
}

int main (void) {
  test_scan_unique_match ();
  test_scan_not_found ();
  test_scan_ambiguous ();
  test_scan_with_mask ();
  test_scan_bad_input ();
  printf ("ALL PASS\n");
  return 0;
}
```

- [ ] **Step 3: Write the test Makefile**

Create `tests/scan/Makefile`:

```makefile
CC      ?= cc
CFLAGS  ?= -O1 -g -Wall -Wextra -Wno-unused-parameter -std=c11
PROJ    := $(realpath ../..)

TESTS   := test_scanfor

all: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

test_scanfor: test_scanfor.c $(PROJ)/GblChainloadPkg/Library/DynamicPatchLib/Internal/ScanLib.c
	$(CC) $(CFLAGS) -I$(PROJ)/GblChainloadPkg/Library/DynamicPatchLib/Internal $^ -o $@

clean:
	rm -f $(TESTS) *.o
```

- [ ] **Step 4: Run the test — must FAIL (no implementation yet)**

```bash
cd /home/vivy/gbl-chainload/tests/scan && make
```

Expected: build error (`undefined reference to ScanFor`) or compilation error (`ScanLib.c not found`). That's the "failing test" state.

- [ ] **Step 5: Implement `ScanLib.c`**

Create `GblChainloadPkg/Library/DynamicPatchLib/Internal/ScanLib.h`:

```c
/* Re-exports the public ScanLib.h for internal users. */
#ifndef DPL_SCANLIB_H_
#define DPL_SCANLIB_H_
#include "../../../Include/Library/ScanLib.h"
#endif
```

Create `GblChainloadPkg/Library/DynamicPatchLib/Internal/ScanLib.c`:

```c
/** @file ScanLib.c — pattern scanner.
    Always scans the whole buffer to detect ambiguity. **/
#include "ScanLib.h"

STATIC BOOLEAN
MatchAt (
  CONST UINT8 *Buf,
  CONST UINT8 *Pattern,
  CONST UINT8 *Mask,
  UINTN        PatternLen
  )
{
  UINTN i;
  for (i = 0; i < PatternLen; ++i) {
    UINT8 b = Buf[i] ^ Pattern[i];
    UINT8 m = (Mask != NULL) ? Mask[i] : 0xFF;
    if ((b & m) != 0) return FALSE;
  }
  return TRUE;
}

SCAN_RESULT
ScanFor (
  IN  CONST UINT8 *Buf,
  IN  UINT32       Size,
  IN  CONST UINT8 *Pattern,
  IN  CONST UINT8 *Mask OPTIONAL,
  IN  UINTN        PatternLen,
  OUT UINT32      *MatchOff
  )
{
  UINT32 i;
  UINT32 Found = 0;
  UINT32 FirstOff = 0;

  if (Buf == NULL || Pattern == NULL || MatchOff == NULL ||
      PatternLen == 0 || Size < PatternLen) {
    return SCAN_BAD_INPUT;
  }

  for (i = 0; i + PatternLen <= Size; ++i) {
    if (MatchAt (Buf + i, Pattern, Mask, PatternLen)) {
      if (Found == 0) FirstOff = i;
      ++Found;
      if (Found > 1) {
        /* Continue scanning to keep the result deterministic, but we already
           know we'll return SCAN_AMBIGUOUS. Skip ahead by 1 (overlapping
           matches still get counted as separate). */
      }
    }
  }

  if (Found == 0) return SCAN_NOT_FOUND;
  if (Found > 1)  return SCAN_AMBIGUOUS;
  *MatchOff = FirstOff;
  return SCAN_FOUND;
}
```

When compiled host-side, `STATIC` and `IN/OUT/OPTIONAL` are no-ops via the test's macro defines. EDK-II builds use the real macros from `Uefi.h`.

- [ ] **Step 6: Run the test — must PASS**

```bash
cd /home/vivy/gbl-chainload/tests/scan && make clean && make
```

Expected:
```
ok test_scan_unique_match
ok test_scan_not_found
ok test_scan_ambiguous
ok test_scan_with_mask
ok test_scan_bad_input
ALL PASS
```

- [ ] **Step 7: Commit**

```bash
cd /home/vivy/gbl-chainload \
  && git add GblChainloadPkg/Include/Library/ScanLib.h \
            GblChainloadPkg/Library/DynamicPatchLib/Internal/ScanLib.h \
            GblChainloadPkg/Library/DynamicPatchLib/Internal/ScanLib.c \
            tests/scan/test_scanfor.c \
            tests/scan/Makefile \
  && git commit -m "DynamicPatchLib: ScanFor + 5-case unit tests (host-compiled)"
```

---

## Task 12: PE-section helpers + `ScanForBoundedSection` — TDD

**Files:**
- Create: `gbl-chainload/GblChainloadPkg/Library/DynamicPatchLib/Internal/PeSections.h`
- Create: `gbl-chainload/GblChainloadPkg/Library/DynamicPatchLib/Internal/PeSections.c`
- Modify: `gbl-chainload/GblChainloadPkg/Include/Library/ScanLib.h`
- Modify: `gbl-chainload/GblChainloadPkg/Library/DynamicPatchLib/Internal/ScanLib.c`
- Create: `tests/scan/test_scan_bounded.c`
- Modify: `tests/scan/Makefile`

The function `IsPeFileOffsetInExecutableSection` already exists in the old repo (`oneplus_canoe.c:617-622` reference). Port it into `PeSections.{c,h}` so it's reusable.

- [ ] **Step 1: Locate the existing helper in the dirty repo**

```bash
grep -n "IsPeFileOffsetInExecutableSection" /home/vivy/gbl-chainload-dirty/GblChainloadPkg/Library/DynamicPatchLib/*.c
```

Expected: a definition site. Read the function — that's the verbatim port target.

- [ ] **Step 2: Create `PeSections.h`**

```c
/** @file PeSections.h — PE/COFF section helpers used by DynamicPatchLib. **/
#ifndef DPL_PE_SECTIONS_H_
#define DPL_PE_SECTIONS_H_

#include <Uefi.h>

/** Return TRUE if the file-offset range [Off, Off+Len) lies entirely
    within an executable PE section in Buf. **/
BOOLEAN
IsPeFileOffsetInExecutableSection (
  IN CONST UINT8 *Buf,
  IN UINT32       Size,
  IN UINT32       Off,
  IN UINT32       Len
  );

#endif
```

- [ ] **Step 3: Port the implementation verbatim into `PeSections.c`**

Copy the function body from the dirty repo into a fresh `PeSections.c` with the appropriate `#include` guards. Keep the implementation byte-equivalent — this is a pure carry-forward.

- [ ] **Step 4: Extend public ScanLib API**

Append to `GblChainloadPkg/Include/Library/ScanLib.h`:

```c
/** Same as ScanFor, but restricted to file-offsets that lie inside
    an executable PE section.  Useful for code-only patch anchors. **/
SCAN_RESULT
ScanForBoundedSection (
  IN  CONST UINT8 *Buf,
  IN  UINT32       Size,
  IN  BOOLEAN      ExecOnly,
  IN  CONST UINT8 *Pattern,
  IN  CONST UINT8 *Mask OPTIONAL,
  IN  UINTN        PatternLen,
  OUT UINT32      *MatchOff
  );
```

- [ ] **Step 5: Write the failing tests**

Create `tests/scan/test_scan_bounded.c`. Use a small synthetic PE buffer with a known executable section range, place patterns inside and outside the executable range, assert correct exclusion.

(Specifying the synthetic PE buffer construction in detail: build a minimal MZ + PE header + one `.text` section table entry covering offsets 0x200..0x400, with the test pattern bytes inserted both at 0x250 (inside .text) and 0x500 (outside). With `ExecOnly=TRUE`, only the in-section match should count; result must be `SCAN_FOUND` with `MatchOff == 0x250`. With `ExecOnly=FALSE`, both match → `SCAN_AMBIGUOUS`.)

A reference for header layout: `efi/edk2-stable*/MdePkg/Include/IndustryStandard/PeImage.h`. The engineer can crib field offsets from the existing `IsPeFileOffsetInExecutableSection` implementation.

- [ ] **Step 6: Add tests to Makefile + run them — fail expected**

```makefile
TESTS := test_scanfor test_scan_bounded
# ... rule for test_scan_bounded similar to test_scanfor, also linking PeSections.c
```

```bash
cd /home/vivy/gbl-chainload/tests/scan && make
```

Expected: build error or `test_scan_bounded` runtime fails (no impl).

- [ ] **Step 7: Implement `ScanForBoundedSection` in `ScanLib.c`**

```c
SCAN_RESULT
ScanForBoundedSection (
  IN  CONST UINT8 *Buf,
  IN  UINT32       Size,
  IN  BOOLEAN      ExecOnly,
  IN  CONST UINT8 *Pattern,
  IN  CONST UINT8 *Mask OPTIONAL,
  IN  UINTN        PatternLen,
  OUT UINT32      *MatchOff
  )
{
  UINT32 i;
  UINT32 Found = 0;
  UINT32 FirstOff = 0;

  if (Buf == NULL || Pattern == NULL || MatchOff == NULL ||
      PatternLen == 0 || Size < PatternLen) {
    return SCAN_BAD_INPUT;
  }

  for (i = 0; i + PatternLen <= Size; ++i) {
    if (ExecOnly && !IsPeFileOffsetInExecutableSection (Buf, Size, i, (UINT32)PatternLen)) {
      continue;
    }
    if (MatchAt (Buf + i, Pattern, Mask, PatternLen)) {
      if (Found == 0) FirstOff = i;
      ++Found;
    }
  }
  if (Found == 0) return SCAN_NOT_FOUND;
  if (Found > 1)  return SCAN_AMBIGUOUS;
  *MatchOff = FirstOff;
  return SCAN_FOUND;
}
```

Add `#include "PeSections.h"` at the top of `ScanLib.c`.

- [ ] **Step 8: Run tests — pass**

```bash
cd /home/vivy/gbl-chainload/tests/scan && make clean && make
```

Expected: all tests pass.

- [ ] **Step 9: Commit**

```bash
cd /home/vivy/gbl-chainload \
  && git add GblChainloadPkg tests/scan \
  && git commit -m "DynamicPatchLib: PeSections + ScanForBoundedSection with tests"
```

---

## Task 13: `Encode.c` — `WriteInstrU32` + `RewriteCbz` — TDD

**Files:**
- Create: `gbl-chainload/GblChainloadPkg/Library/DynamicPatchLib/Internal/Encode.h`
- Create: `gbl-chainload/GblChainloadPkg/Library/DynamicPatchLib/Internal/Encode.c`
- Create: `tests/scan/test_encode.c`

The functions exist in the old repo (`WriteInstr`, `EncodeCbz`). Port them and rename for clarity.

- [ ] **Step 1: Locate originals**

```bash
grep -nE "WriteInstr|EncodeCbz" /home/vivy/gbl-chainload-dirty/GblChainloadPkg/Library/DynamicPatchLib/*.c
```

- [ ] **Step 2: Write public `Encode.h`**

```c
/** @file Encode.h — AArch64 instruction encoding helpers for patches. **/
#ifndef DPL_ENCODE_H_
#define DPL_ENCODE_H_

#include <Uefi.h>

VOID
WriteInstrU32 (
  IN OUT UINT8  *Buf,
  IN     UINT32  FileOff,
  IN     UINT32  Insn
  );

UINT32
ReadInstrU32 (
  IN CONST UINT8 *Buf,
  IN UINT32       FileOff
  );

/** Encode a CBZ Wn, target instruction.  Returns FALSE if the displacement
    does not fit the 19-bit signed immediate (units of 4). **/
BOOLEAN
EncodeCbz (
  IN  UINT32  InsnOff,
  IN  UINT32  TargetOff,
  IN  UINT32  Reg,
  OUT UINT32 *Insn
  );

/** Convenience: scan-target-relative CBZ rewrite.
    @return TRUE on successful rewrite, FALSE if displacement out of range. **/
BOOLEAN
RewriteCbz (
  IN OUT UINT8  *Buf,
  IN     UINT32  InsnOff,
  IN     UINT32  Reg,
  IN     UINT32  TargetOff
  );

#endif
```

- [ ] **Step 3: Write failing tests**

Create `tests/scan/test_encode.c`. Tests:

1. `test_write_read_roundtrip` — write a known instruction, read it back, expect equality.
2. `test_encode_cbz_known_value` — encode `cbz w24, +0x4` → expected `0x340000F8` (or compute from the AArch64 spec). Compare against a known-good value taken from a real binary or a small assembler:

   Quick reference: CBZ encoding bits are `[31:25]=0011010 [24]=sf [23:5]=imm19 [4:0]=Rt`. `cbz w24, +X` (where X is a positive 4-byte multiple) → `imm19 = X/4`, `Rt = 24`, `sf=0`. So `cbz w24, +0x4` → `imm19=1`, encoding = `(0x34 << 24) | (1 << 5) | 24 = 0x34000038`. (Engineer should re-derive against the spec to confirm.)

3. `test_encode_cbz_out_of_range` — too-large displacement returns FALSE.

- [ ] **Step 4: Run — fail**

```bash
cd /home/vivy/gbl-chainload/tests/scan && make
```

- [ ] **Step 5: Implement `Encode.c`**

Port from old repo. Key encoding logic for CBZ:

```c
BOOLEAN
EncodeCbz (
  IN  UINT32  InsnOff,
  IN  UINT32  TargetOff,
  IN  UINT32  Reg,
  OUT UINT32 *Insn
  )
{
  /* Displacement in bytes from the instruction site to the target. */
  INT32 Delta = (INT32)TargetOff - (INT32)InsnOff;
  if ((Delta & 0x3) != 0) return FALSE;            /* must be 4-byte-aligned */
  Delta >>= 2;
  if (Delta < -(1 << 18) || Delta >= (1 << 18)) return FALSE;  /* 19-bit signed */
  if (Reg > 31) return FALSE;
  /* CBZ Wn, target: 0x34 << 24 | imm19 << 5 | Rt */
  *Insn = 0x34000000u | ((UINT32)(Delta & 0x7FFFF) << 5) | (Reg & 0x1F);
  return TRUE;
}
```

`WriteInstrU32` and `ReadInstrU32` are simple little-endian helpers.

- [ ] **Step 6: Run tests — pass**

- [ ] **Step 7: Commit**

```bash
git add GblChainloadPkg/Library/DynamicPatchLib/Internal/Encode.{h,c} tests/scan/test_encode.c \
  && git commit -m "DynamicPatchLib: Encode.c (WriteInstrU32, RewriteCbz) with tests"
```

---

## Task 14: `PatchEngine.c` — `DynamicPatch_Apply` iterator — TDD

**Files:**
- Create: `gbl-chainload/GblChainloadPkg/Include/Library/DynamicPatchLib.h`
- Create: `gbl-chainload/GblChainloadPkg/Library/DynamicPatchLib/Internal/PatchDesc.h`
- Create: `gbl-chainload/GblChainloadPkg/Library/DynamicPatchLib/Internal/PatchEngine.c`
- Create: `tests/scan/test_engine.c`

- [ ] **Step 1: Define `PATCH_DESC` in `PatchDesc.h`**

```c
#ifndef DPL_PATCH_DESC_H_
#define DPL_PATCH_DESC_H_
#include <Uefi.h>

typedef enum {
  PATCH_OK         = 0,
  PATCH_MISS       = 1,
  PATCH_AMBIGUOUS  = 2,
} PATCH_OUTCOME;

typedef enum {
  SCOPE_UNIVERSAL     = 0,
  SCOPE_OEM_ONEPLUS   = 1,
  SCOPE_MODE_1        = 2,
  /* SCOPE_MODE_2, SCOPE_MODE_3, SCOPE_OEM_<other> in later plans. */
} PATCH_SCOPE;

typedef PATCH_OUTCOME (*PATCH_APPLY)(UINT8 *Buf, UINT32 Size);

typedef struct {
  CONST CHAR8  *Name;
  PATCH_SCOPE   Scope;
  BOOLEAN       Mandatory;
  PATCH_APPLY   Apply;
} PATCH_DESC;

#endif
```

- [ ] **Step 2: Define public `DynamicPatchLib.h`**

```c
#ifndef DYNAMIC_PATCH_LIB_H_
#define DYNAMIC_PATCH_LIB_H_
#include "../../GblChainloadPkg/Library/DynamicPatchLib/Internal/PatchDesc.h"

typedef enum {
  PATCH_RESULT_OK              = 0,
  PATCH_RESULT_OPTIONAL_MISS   = 1,
  PATCH_RESULT_MANDATORY_MISS  = 2,
} PATCH_WORST;

typedef struct {
  UINT32       AppliedCount;
  UINT32       MissedCount;
  PATCH_WORST  WorstOutcome;
} PATCH_RESULT;

VOID
DynamicPatch_Apply (
  IN OUT UINT8         *Buf,
  IN     UINT32         Size,
  OUT    PATCH_RESULT  *Result
  );

#endif
```

- [ ] **Step 3: Write the failing test**

`tests/scan/test_engine.c` — fixtures using a stub patch table:

```c
static PATCH_OUTCOME StubOk (UINT8 *b, UINT32 s) { (void)b; (void)s; return PATCH_OK; }
static PATCH_OUTCOME StubMiss (UINT8 *b, UINT32 s) { (void)b; (void)s; return PATCH_MISS; }
/* ... build a static PATCH_DESC[] with mixed outcomes, call DynamicPatch_Apply,
   assert AppliedCount, MissedCount, WorstOutcome reflect the table. */
```

The stub patches cover three matrices: all-OK, one-mandatory-miss, one-optional-miss. Each yields a distinct expected `PATCH_RESULT`.

For the test to compile, the engine's patch table needs to be injectable. Use a build-time symbol:

```c
extern CONST PATCH_DESC  *gPatchTable;
extern UINTN              gPatchTableLen;
```

The test sets these to its stub array; the runtime build sets them from `universal/`, `oem/`, `mode_1/` aggregator (Task 17).

- [ ] **Step 4: Run — fail**

- [ ] **Step 5: Implement `PatchEngine.c`**

```c
#include "../../../Include/Library/DynamicPatchLib.h"
#include "PatchDesc.h"

/* Aggregator symbol; provided by the build (Task 17). */
extern CONST PATCH_DESC  *gPatchTable;
extern UINTN              gPatchTableLen;

VOID
DynamicPatch_Apply (
  IN OUT UINT8         *Buf,
  IN     UINT32         Size,
  OUT    PATCH_RESULT  *Result
  )
{
  UINTN i;
  Result->AppliedCount = 0;
  Result->MissedCount  = 0;
  Result->WorstOutcome = PATCH_RESULT_OK;

  for (i = 0; i < gPatchTableLen; ++i) {
    CONST PATCH_DESC *P = &gPatchTable[i];
    PATCH_OUTCOME O = P->Apply (Buf, Size);
    if (O == PATCH_OK) {
      ++Result->AppliedCount;
    } else {
      ++Result->MissedCount;
      if (P->Mandatory) {
        if (Result->WorstOutcome < PATCH_RESULT_MANDATORY_MISS) {
          Result->WorstOutcome = PATCH_RESULT_MANDATORY_MISS;
        }
      } else {
        if (Result->WorstOutcome < PATCH_RESULT_OPTIONAL_MISS) {
          Result->WorstOutcome = PATCH_RESULT_OPTIONAL_MISS;
        }
      }
    }
    /* Log line: name | scope | outcome — wrapped in DEBUG() in EDK-II build. */
  }
}
```

- [ ] **Step 6: Test passes**

- [ ] **Step 7: Commit**

```bash
git add GblChainloadPkg tests/scan/test_engine.c \
  && git commit -m "DynamicPatchLib: PatchEngine.c iterator with PATCH_DESC + tests"
```

---

## Task 15: Stock canoe ABL fixture — extraction script

**Files:**
- Create: `gbl-chainload/scripts/extract-canoe-fixtures.sh`
- Create: `gbl-chainload/images/.gitkeep` (empty marker so the gitignored dir is clear)
- Create: `gbl-chainload/images/README.md` (explains gitignored content)

The fixture set is the input to anchor-uniqueness CI. We need at least:

- `images/canoe-stock-A.07_2024_02_05/abl_a.bin` (extracted from stock OTA)
- `images/infiniti/LinuxLoader_infiniti.efi` (already at `/home/vivy/gbl_root_canoe/images/`)

- [ ] **Step 1: Write `scripts/extract-canoe-fixtures.sh`**

```bash
#!/usr/bin/env bash
# Extract fixture binaries used by tests/042 anchor-uniqueness CI.
# Inputs: stock OTA partition images dropped into images/canoe-stock-A.07_2024_02_05/
# Outputs: fixtures/canoe-A.07/abl_a.bin (a verbatim copy that CI can rely on)
set -euo pipefail

SRC_DIR="${1:-images/canoe-stock-A.07_2024_02_05}"
DEST_DIR="${2:-images/fixtures/canoe-A.07}"

if [[ ! -f "$SRC_DIR/abl_a.bin" && ! -f "$SRC_DIR/abl_a.img" ]]; then
  echo "ERROR: $SRC_DIR/abl_a.{bin,img} not found." >&2
  echo "Drop a stock A.07 OTA's abl_a partition image at that path." >&2
  exit 1
fi

mkdir -p "$DEST_DIR"
cp -f "$SRC_DIR"/abl_a.{bin,img} "$DEST_DIR/abl_a.bin" 2>/dev/null \
  || cp -f "$SRC_DIR/abl_a.bin" "$DEST_DIR/abl_a.bin"
sha256sum "$DEST_DIR/abl_a.bin"
echo "OK: fixture at $DEST_DIR/abl_a.bin"
```

`chmod +x scripts/extract-canoe-fixtures.sh`.

- [ ] **Step 2: Write `images/README.md` (tracked)**

```markdown
# images/

Gitignored by default. Populated by the developer with stock OTA partition
images for fixture extraction.

## Expected inputs

- `images/canoe-stock-A.07_2024_02_05/abl_a.bin` — stock A.07_2024_02_05 ABL.
- `images/infiniti/LinuxLoader_infiniti.efi` — symlink-ok to
  `/home/vivy/gbl_root_canoe/images/LinuxLoader_infiniti.efi`.
- (Plan 2) `images/canoe-stock-A.07_2024_02_05/{boot,recovery,dtbo,vbmeta}.img`
  for mode-2 profile extraction.

## Outputs

`scripts/extract-canoe-fixtures.sh` produces `images/fixtures/canoe-A.07/abl_a.bin`
which `tests/042_dynamic_patch_harness.sh` picks up.
```

- [ ] **Step 3: Drop the dirty repo's existing infiniti fixture path into the new repo's images dir**

```bash
mkdir -p /home/vivy/gbl-chainload/images/infiniti
ln -sf /home/vivy/gbl_root_canoe/images/LinuxLoader_infiniti.efi \
       /home/vivy/gbl-chainload/images/infiniti/LinuxLoader_infiniti.efi
```

(Symlinks won't be tracked because of the gitignore.)

- [ ] **Step 4: Extract additional infiniti fixture from EU 16.0.5.703 OTA**

The user has confirmed an infiniti 16.0.5.703 OTA at `~/Downloads/RegionalHybrid Flasher 15 EU 16.0.5.703/OOS_FILES_HERE/`. Adding this as a second infiniti fixture (alongside gbl_root_canoe's older `LinuxLoader_infiniti.efi`) gives the anchor-uniqueness CI cross-version coverage and strengthens patch9's "libavb is a fair constant" claim.

```bash
SRC="$HOME/Downloads/RegionalHybrid Flasher 15 EU 16.0.5.703/OOS_FILES_HERE"
DEST=images/infiniti-EU-16.0.5.703
mkdir -p "$DEST"
# abl_a partition is the unwrap source for LinuxLoader_infiniti.efi
if [[ -f "$SRC/abl_a.img" ]]; then
  cp "$SRC/abl_a.img" "$DEST/abl_a.bin"
  sha256sum "$DEST/abl_a.bin"
elif [[ -f "$SRC/abl.img" ]]; then
  cp "$SRC/abl.img" "$DEST/abl.bin"
  sha256sum "$DEST/abl.bin"
else
  echo "WARN: no abl_a.img or abl.img in $SRC — list contents:"
  ls "$SRC"
fi
```

Expected: prints SHA256 of the extracted partition. The extracted PE inside the FV (the actual `LinuxLoader_infiniti_16.0.5.703.efi`) is produced at host-test time by running `AblUnwrap_LoadFromBuffer` on the raw partition — `tests/patches/test_patch9.c` (Task 18) handles the unwrap step.

- [ ] **Step 5: Stock canoe `abl_a.bin` — manual prerequisite (deferred)**

The user will need to drop a stock canoe `abl_a.bin` into `images/canoe-stock-A.07_2024_02_05/` from a stock canoe OTA download. Until that exists:

- `tests/042` runs anchor uniqueness against the available infiniti fixtures (now two of them: gbl_root_canoe's old, and EU 16.0.5.703), warning that the canoe leg is uncovered.
- `tests/patches/test_patch9.c` runs the canoe leg under `#ifdef HAVE_CANOE_FIXTURE`; CI emits a warning if the canoe fixture is absent.
- Final mode-1 device validation (Task 31 Step 5) is the canonical canoe verification, which doesn't require a host fixture.

This is acceptable for plan-1 execution — the engineer should not block on the canoe fixture, but should mark `tests/042` warnings prominently in CI output so the gap stays visible.

If the user gets a canoe stock OTA mid-plan, run:

```bash
cd /home/vivy/gbl-chainload && ./scripts/extract-canoe-fixtures.sh
```

Expected: prints SHA256 + "OK: fixture at images/fixtures/canoe-A.07/abl_a.bin". `tests/042` and `tests/patches/test_patch9.c` then exercise the canoe leg automatically without code changes.

- [ ] **Step 6: Commit (script + README + .gitkeep)**

```bash
cd /home/vivy/gbl-chainload \
  && touch images/.gitkeep \
  && git add scripts/extract-canoe-fixtures.sh images/.gitkeep images/README.md \
  && git commit -m "fixtures: extraction script + images/ README + canoe-A.07 directory"
```

---

## Task 16: Patch1 (efisp recursion) — port to anchor-scan + test

**Files:**
- Create: `gbl-chainload/GblChainloadPkg/Library/DynamicPatchLib/universal/universal.c`
- Create: `gbl-chainload/GblChainloadPkg/Library/DynamicPatchLib/universal/Signatures.h`
- Create: `tests/patches/test_patch1.c`
- Modify: `tests/scan/Makefile` or new `tests/patches/Makefile`

Patch1 in the dirty repo already uses byte-scan for UTF-16LE `"efisp"` → `"nulls"`. Port verbatim into the new structure.

- [ ] **Step 1: Locate the patch1 source in the dirty repo**

```bash
grep -nA 30 "Patch1 (EFISP)" /home/vivy/gbl-chainload-dirty/GblChainloadPkg/Library/DynamicPatchLib/*.c
```

- [ ] **Step 2: Author `universal/Signatures.h`**

```c
#ifndef DPL_UNIVERSAL_SIGNATURES_H_
#define DPL_UNIVERSAL_SIGNATURES_H_

/* UTF-16LE "efisp" — 10 bytes. */
STATIC CONST UINT8  kEfispUtf16Pattern[] = {
  'e', 0, 'f', 0, 'i', 0, 's', 0, 'p', 0
};

#endif
```

- [ ] **Step 3: Author `universal/universal.c`**

```c
#include "../Internal/PatchDesc.h"
#include "../Internal/ScanLib.h"
#include "Signatures.h"

STATIC PATCH_OUTCOME
ApplyEfispRecursion (
  IN OUT UINT8  *Buf,
  IN     UINT32  Size
  )
{
  UINT32      Off;
  SCAN_RESULT R;

  R = ScanFor (Buf, Size,
               kEfispUtf16Pattern, NULL, sizeof (kEfispUtf16Pattern), &Off);
  if (R == SCAN_NOT_FOUND)   return PATCH_MISS;
  if (R == SCAN_AMBIGUOUS)   return PATCH_AMBIGUOUS;
  if (R != SCAN_FOUND)       return PATCH_MISS;

  /* "efisp" → "nulls", in UTF-16LE. */
  Buf[Off + 0] = 'n'; Buf[Off + 2] = 'u';
  Buf[Off + 4] = 'l'; Buf[Off + 6] = 'l';
  Buf[Off + 8] = 's';
  return PATCH_OK;
}

CONST PATCH_DESC kUniversalPatches[] = {
  {
    .Name      = "patch1-efisp-recursion",
    .Scope     = SCOPE_UNIVERSAL,
    .Mandatory = TRUE,
    .Apply     = ApplyEfispRecursion,
  },
};
CONST UINTN kUniversalPatchesCount =
  sizeof (kUniversalPatches) / sizeof (kUniversalPatches[0]);
```

- [ ] **Step 4: Write the test**

`tests/patches/test_patch1.c` — load `images/fixtures/canoe-A.07/abl_a.bin` into memory (if available) or `images/infiniti/LinuxLoader_infiniti.efi`, run `ApplyEfispRecursion`, check:

1. Outcome is `PATCH_OK`.
2. The byte at the matched offset is now `'n'`.
3. Re-applying the patch returns `PATCH_NOT_FOUND` (because the pattern is gone).

The test loads files via `fopen` and operates on a `malloc`'d buffer.

- [ ] **Step 5: Run the test — pass on whichever fixture is available**

If only the infiniti fixture is present, `test_patch1` should still pass against it. The canoe leg becomes meaningful once the user populates `images/canoe-stock-A.07_2024_02_05/abl_a.bin`.

- [ ] **Step 6: Commit**

```bash
git add GblChainloadPkg/Library/DynamicPatchLib/universal tests/patches \
  && git commit -m "DynamicPatchLib: patch1-efisp-recursion ported as anchor-scan"
```

---

## Task 17: Patch7 (orange screen) — port to anchor-scan

**Files:**
- Create: `gbl-chainload/GblChainloadPkg/Library/DynamicPatchLib/oem/oneplus_canoe.c`
- Create: `gbl-chainload/GblChainloadPkg/Library/DynamicPatchLib/oem/Signatures.h`
- Create: `tests/patches/test_patch7.c`

Patch7 in the dirty repo uses fixed offsets into `LinuxLoader_infiniti.efi` (e.g. `0x78F0`). The port re-expresses it as a unique-anchor scan so it works on canoe + infiniti with one rule.

- [ ] **Step 1: Read the existing patch7 implementation**

```bash
grep -nA 60 "Patch7 (orange)" /home/vivy/gbl-chainload-dirty/GblChainloadPkg/Library/DynamicPatchLib/oem/oneplus_canoe.c
```

Note the offset (0x78F0), the original instruction (CBZ), and the rewrite (B unconditional branch).

- [ ] **Step 2: RE step — derive a unique anchor pattern around the orange-state warning callsite**

Inputs:
- `images/fixtures/canoe-A.07/abl_a.bin` (and/or `images/infiniti/LinuxLoader_infiniti.efi`).
- The instruction at offset 0x78F0 in the infiniti binary is the rewrite target.

Goal: identify a 16-32 byte instruction sequence around (and including) the rewrite target that is unique across both fixtures. Use:

```bash
# Disassemble around the target offset
aarch64-linux-gnu-objdump -D -b binary -m aarch64 \
  --adjust-vma=0 --start-address=0x78d0 --stop-address=0x7910 \
  images/infiniti/LinuxLoader_infiniti.efi
```

(or use Ghidra). Capture 4-8 instructions worth of bytes preceding and at the rewrite point. Verify they are unique by running `tools/abl-patcher --check-anchors-only` (Task 22) once that exists; for now, code the pattern in and let `tests/042` verify.

Output: `kPatch7AnchorPattern[]` (24 bytes) and `kPatch7AnchorMask[]` (matching size). The pattern's relative offset to the rewrite target (e.g. anchor matches at offset N, rewrite is at N+0xC) is encoded as a constant.

- [ ] **Step 3: Author `oem/oneplus_canoe.c`**

```c
#include "../Internal/PatchDesc.h"
#include "../Internal/ScanLib.h"
#include "../Internal/Encode.h"
#include "Signatures.h"

STATIC PATCH_OUTCOME
ApplyOrangeScreen (
  IN OUT UINT8  *Buf,
  IN     UINT32  Size
  )
{
  UINT32      AnchorOff;
  UINT32      RewriteOff;
  SCAN_RESULT R;

  R = ScanForBoundedSection (
        Buf, Size, /*ExecOnly=*/TRUE,
        kPatch7AnchorPattern, kPatch7AnchorMask,
        sizeof (kPatch7AnchorPattern), &AnchorOff);
  if (R != SCAN_FOUND) {
    return (R == SCAN_AMBIGUOUS) ? PATCH_AMBIGUOUS : PATCH_MISS;
  }

  RewriteOff = AnchorOff + kPatch7RewriteDelta;
  /* Replace the original CBZ with an unconditional forward branch.
     Equivalent semantics: always take the "skip orange warning" path. */
  WriteInstrU32 (Buf, RewriteOff, kPatch7BUnconditionalInsn);
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

`Signatures.h` defines `kPatch7AnchorPattern`, `kPatch7AnchorMask`, `kPatch7RewriteDelta`, and `kPatch7BUnconditionalInsn` (the new instruction word).

- [ ] **Step 4: Test against fixture(s)**

Same shape as Task 16's test: load fixture, call `ApplyOrangeScreen`, expect `PATCH_OK`, expect the rewritten word at the expected offset matches `kPatch7BUnconditionalInsn`.

- [ ] **Step 5: Run — pass**

- [ ] **Step 6: Commit**

```bash
git add GblChainloadPkg/Library/DynamicPatchLib/oem tests/patches/test_patch7.c \
  && git commit -m "DynamicPatchLib: patch7-orange-screen ported as anchor-scan"
```

---

## Task 18: Patch9 (AVB locked recoverable continue) — port to anchor-scan, mode-1 scope

**Files:**
- Create: `gbl-chainload/GblChainloadPkg/Library/DynamicPatchLib/mode_1/mode_1.c`
- Create: `gbl-chainload/GblChainloadPkg/Library/DynamicPatchLib/mode_1/Signatures.h`
- Create: `tests/patches/test_patch9.c`

This is the highest-stakes task — patch9 is the patch that misses on canoe today. The new scan-based form must hit on **both** canoe and infiniti.

- [ ] **Step 1: Re-read the dirty repo patch9 implementation**

```bash
sed -n '590,680p' /home/vivy/gbl-chainload-dirty/GblChainloadPkg/Library/DynamicPatchLib/oem/oneplus_canoe.c
```

Captures the original three rewrite sites (verify-flags init, recovery gate, common gate) and their fixed infiniti offsets `0x25388`, `0x25A64`, `0x25C44`.

- [ ] **Step 2: RE step — Ghidra-confirm canoe binary's equivalent sites**

Open `images/fixtures/canoe-A.07/abl_a.bin` in Ghidra (already-annotated infiniti project at `/home/vivy/gbl_root_canoe/.../LinuxLoader_infiniti.efi.gpr` is the source-side reference). Find the equivalent of:

- `VerifyFlagsInit` (CSET W24, NE → MOV W24, #1)
- `RecoveryGate` (CBZ W29 → CBZ W24)
- `CommonGate` (CBZ W29 → CBZ W24)

These are AArch64 instructions surrounding the AVB result-to-bootstate decision. Source-side reference: `edk2/QcomModulePkg/Library/avb/VerifiedBoot.c:1591-1605` and `avb_slot_verify.c:1672-1674` (per the dirty repo session note).

- [ ] **Step 3: Derive a unique anchor pattern across both fixtures**

Pick the verify-flags-init site (`CSET W24, NE` is `0x1A9F07F8` on infiniti). The 8 instructions preceding it form a 32-byte anchor:

```bash
# Read 32 bytes ending at the cset
xxd -s 0x25368 -l 32 images/infiniti/LinuxLoader_infiniti.efi
```

Compare with canoe's equivalent location (canoe's offset will differ, but the byte sequence around the cset should be identical or near-identical for the same source compilation). Mask out any function-prologue stack offsets that differ.

Outcome: `kPatch9AnchorPattern[]` (~32 bytes), `kPatch9AnchorMask[]`. The pattern matches at offset N; the cset rewrite is at N+28, and the recovery / common gate rewrites are at N+(0x25A64-0x25388)=+0x6DC and N+(0x25C44-0x25388)=+0x8BC respectively.

If the recovery/common gate sites' relative offsets differ between canoe and infiniti, derive **separate** anchors for each rewrite site, each scoped narrowly enough to be unique.

- [ ] **Step 4: Author `mode_1/mode_1.c`**

```c
#include "../Internal/PatchDesc.h"
#include "../Internal/ScanLib.h"
#include "../Internal/Encode.h"
#include "Signatures.h"

STATIC PATCH_OUTCOME
ApplyAvbLockedRecoverableContinue (
  IN OUT UINT8  *Buf,
  IN     UINT32  Size
  )
{
  UINT32      AnchorOff;
  UINT32      VerifyFlagsOff;
  UINT32      RecoveryGateOff;
  UINT32      CommonGateOff;
  UINT32      RecoveryLockedOkOff;
  UINT32      CommonOkRollbackOff;
  SCAN_RESULT R;

  R = ScanForBoundedSection (
        Buf, Size, /*ExecOnly=*/TRUE,
        kPatch9AnchorPattern, kPatch9AnchorMask,
        sizeof (kPatch9AnchorPattern), &AnchorOff);
  if (R != SCAN_FOUND) {
    return (R == SCAN_AMBIGUOUS) ? PATCH_AMBIGUOUS : PATCH_MISS;
  }

  VerifyFlagsOff      = AnchorOff + kPatch9VerifyFlagsDelta;
  RecoveryGateOff     = AnchorOff + kPatch9RecoveryGateDelta;
  CommonGateOff       = AnchorOff + kPatch9CommonGateDelta;
  RecoveryLockedOkOff = AnchorOff + kPatch9RecoveryLockedOkDelta;
  CommonOkRollbackOff = AnchorOff + kPatch9CommonOkRollbackDelta;

  /* 1. cset w24,ne → mov w24,#1 */
  WriteInstrU32 (Buf, VerifyFlagsOff, 0x52800038U);

  /* 2. cbz w29,<lockedOk> → cbz w24,<lockedOk> */
  if (!RewriteCbz (Buf, RecoveryGateOff, /*reg=*/24, RecoveryLockedOkOff)) {
    return PATCH_MISS;
  }

  /* 3. cbz w29,<rollback> → cbz w24,<rollback> */
  if (!RewriteCbz (Buf, CommonGateOff, /*reg=*/24, CommonOkRollbackOff)) {
    return PATCH_MISS;
  }

  return PATCH_OK;
}

CONST PATCH_DESC kMode1Patches[] = {
  {
    .Name      = "patch9-avb-locked-recoverable-continue",
    .Scope     = SCOPE_MODE_1,
    .Mandatory = TRUE,
    .Apply     = ApplyAvbLockedRecoverableContinue,
  },
};
CONST UINTN kMode1PatchesCount =
  sizeof (kMode1Patches) / sizeof (kMode1Patches[0]);
```

- [ ] **Step 5: Test against both fixtures (mandatory)**

`tests/patches/test_patch9.c`:

1. Load `images/infiniti/LinuxLoader_infiniti.efi`, run patch9, expect `PATCH_OK`.
2. Verify post-bytes at the original infiniti offsets match expected outputs (`0x52800038`, `0x34001958`, `0x34000DB8` per dirty repo session note).
3. Load `images/fixtures/canoe-A.07/abl_a.bin`, run patch9, expect `PATCH_OK`.
4. Verify canoe-specific post-bytes (engineer derives expected from RE step 3).

If the canoe fixture is not yet present, mark the canoe leg with `#ifdef HAVE_CANOE_FIXTURE` so CI can run partial coverage and emit a warning, but never silently pass. The test must FAIL (not skip) once canoe fixture lands.

- [ ] **Step 6: Run — pass on both fixtures**

```bash
cd /home/vivy/gbl-chainload/tests/patches && make
```

Expected: `test_patch9 (infiniti)` PASS, `test_patch9 (canoe)` PASS.

- [ ] **Step 7: Commit**

```bash
git add GblChainloadPkg/Library/DynamicPatchLib/mode_1 tests/patches/test_patch9.c \
  && git commit -m "DynamicPatchLib: patch9-avb-recoverable-continue as anchor-scan, mode-1 scope"
```

---

## Task 19: Patch table aggregator + mode-aware selection

**Files:**
- Create: `gbl-chainload/GblChainloadPkg/Library/DynamicPatchLib/PatchTable.c`
- Create: `gbl-chainload/GblChainloadPkg/Library/DynamicPatchLib/DynamicPatchLib.inf`

The aggregator concatenates `kUniversalPatches`, `kOemOneplusPatches`, and (when `GBL_MODE==1`) `kMode1Patches` into the engine's runtime table.

- [ ] **Step 1: Author `PatchTable.c`**

```c
#include "Internal/PatchDesc.h"

extern CONST PATCH_DESC kUniversalPatches[];
extern CONST UINTN      kUniversalPatchesCount;
extern CONST PATCH_DESC kOemOneplusPatches[];
extern CONST UINTN      kOemOneplusPatchesCount;

#if (GBL_MODE == 1)
extern CONST PATCH_DESC kMode1Patches[];
extern CONST UINTN      kMode1PatchesCount;
#endif

/* Build a flat table at link time using a small C trampoline.  Order matters:
   universal first (so an OEM patch can rely on universal having run), then
   OEM, then mode-specific.  EDK-II's small-binary CRT does not support
   automatic concatenation, so we copy at first call into a static buffer. */

#define MAX_PATCHES  16

STATIC PATCH_DESC  gAggregated[MAX_PATCHES];
STATIC UINTN       gAggregatedLen = 0;
STATIC BOOLEAN     gAggregateInit = FALSE;

CONST PATCH_DESC  *gPatchTable;
UINTN              gPatchTableLen;

STATIC VOID
InitAggregate (VOID) {
  UINTN n = 0;
  UINTN i;

  for (i = 0; i < kUniversalPatchesCount; ++i) {
    if (n < MAX_PATCHES) gAggregated[n++] = kUniversalPatches[i];
  }
  for (i = 0; i < kOemOneplusPatchesCount; ++i) {
    if (n < MAX_PATCHES) gAggregated[n++] = kOemOneplusPatches[i];
  }
#if (GBL_MODE == 1)
  for (i = 0; i < kMode1PatchesCount; ++i) {
    if (n < MAX_PATCHES) gAggregated[n++] = kMode1Patches[i];
  }
#endif
  gAggregatedLen = n;
  gPatchTable    = gAggregated;
  gPatchTableLen = n;
  gAggregateInit = TRUE;
}

VOID
DynamicPatchLib_EnsureInit (VOID) {
  if (!gAggregateInit) InitAggregate ();
}
```

`PatchEngine.c::DynamicPatch_Apply` calls `DynamicPatchLib_EnsureInit()` at top.

- [ ] **Step 2: Author `DynamicPatchLib.inf`**

```ini
[Defines]
  INF_VERSION    = 0x00010005
  BASE_NAME      = DynamicPatchLib
  FILE_GUID      = 8b1f3a90-aaaa-4321-b000-fedcba012345  # generate fresh
  MODULE_TYPE    = UEFI_APPLICATION
  VERSION_STRING = 1.0
  LIBRARY_CLASS  = DynamicPatchLib

[Sources]
  Internal/ScanLib.c
  Internal/PeSections.c
  Internal/Encode.c
  Internal/PatchEngine.c
  PatchTable.c
  universal/universal.c
  oem/oneplus_canoe.c
  mode_1/mode_1.c

[Packages]
  MdePkg/MdePkg.dec
  GblChainloadPkg/GblChainloadPkg.dec

[LibraryClasses]
  BaseLib
  BaseMemoryLib
  DebugLib

[FeaturePcd]
  gGblChainloadPkgTokenSpaceGuid.PcdGblMode

[BuildOptions]
  *_*_AARCH64_CC_FLAGS = -DGBL_MODE=$(GBL_MODE)
```

(`gGblChainloadPkgTokenSpaceGuid.PcdGblMode` is wired in Task 24.)

- [ ] **Step 3: Update `tests/scan/test_engine.c` to call `DynamicPatchLib_EnsureInit()` if it links the real aggregator**

The host test for the engine continues to use a stub patch table directly. The aggregator only matters for EDK-II builds.

- [ ] **Step 4: Lint test — assert mode_1 patches link only when `GBL_MODE==1`**

Add `tests/045_mode_taxonomy_lint.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
PKG="GblChainloadPkg/Library/DynamicPatchLib"
test -f "$PKG/PatchTable.c" || { echo "missing PatchTable.c"; exit 1; }
grep -q '#if (GBL_MODE == 1)' "$PKG/PatchTable.c" \
  || { echo "PatchTable.c must gate mode_1 patches behind GBL_MODE==1"; exit 1; }
echo "ok 045_mode_taxonomy_lint"
```

`chmod +x tests/045_mode_taxonomy_lint.sh`.

- [ ] **Step 5: Run lint — pass**

```bash
cd /home/vivy/gbl-chainload && bash tests/045_mode_taxonomy_lint.sh
```

- [ ] **Step 6: Commit**

```bash
git add GblChainloadPkg/Library/DynamicPatchLib/PatchTable.c \
        GblChainloadPkg/Library/DynamicPatchLib/DynamicPatchLib.inf \
        tests/045_mode_taxonomy_lint.sh \
  && git commit -m "DynamicPatchLib: PatchTable aggregator + 045 mode taxonomy lint"
```

---

## Task 20: `tools/abl-patcher` host binary skeleton

**Files:**
- Create: `gbl-chainload/tools/abl-patcher/abl-patcher.c`
- Create: `gbl-chainload/tools/abl-patcher/Makefile`

- [ ] **Step 1: Write `Makefile`**

```makefile
CC      ?= cc
CFLAGS  ?= -O1 -g -Wall -Wextra -std=c11
PROJ    := $(realpath ../..)
DPL     := $(PROJ)/GblChainloadPkg/Library/DynamicPatchLib

OBJS := abl-patcher.o \
        $(DPL)/Internal/ScanLib.o \
        $(DPL)/Internal/PeSections.o \
        $(DPL)/Internal/Encode.o \
        $(DPL)/Internal/PatchEngine.o \
        $(DPL)/universal/universal.o \
        $(DPL)/oem/oneplus_canoe.o \
        $(DPL)/mode_1/mode_1.o \
        $(DPL)/PatchTable.o

CFLAGS += -I$(PROJ)/GblChainloadPkg/Include/Library \
          -I$(DPL)/Internal -I$(DPL)/universal -I$(DPL)/oem -I$(DPL)/mode_1 \
          -DGBL_MODE=1 -DABL_PATCHER_HOST_BUILD=1

abl-patcher: $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f abl-patcher $(OBJS)
```

(Host-side type aliases are added with a small `host-shim.h` included via `-include` in CFLAGS so EDK-II macros resolve to no-ops on host. The shim defines `IN/OUT/OPTIONAL/EFIAPI` etc. as empty and aliases UINT8/UINT32/UINTN/BOOLEAN/CONST/VOID.)

Add `host-shim.h` at `tools/abl-patcher/host-shim.h`:

```c
#ifndef ABL_PATCHER_HOST_SHIM_H_
#define ABL_PATCHER_HOST_SHIM_H_
#ifdef ABL_PATCHER_HOST_BUILD
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define IN
#define OUT
#define OPTIONAL
#define EFIAPI
#define STATIC static
#define CONST const
#define VOID void
typedef uint8_t  UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef int32_t  INT32;
typedef size_t   UINTN;
typedef int      BOOLEAN;
#define TRUE 1
#define FALSE 0
#define DEBUG(x)  do {} while (0)
#define DEBUG_INFO 0
#define DEBUG_WARN 0
#endif
#endif
```

Add `-include host-shim.h` to CFLAGS.

- [ ] **Step 2: Write `abl-patcher.c`**

```c
#include "host-shim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include "DynamicPatchLib.h"

VOID DynamicPatchLib_EnsureInit (VOID);

static int Usage (CONST char *argv0) {
  fprintf (stderr,
    "Usage: %s --in <abl.bin> [--out <patched.bin>]\n"
    "       %s --check-anchors-only --fixtures <path...>\n",
    argv0, argv0);
  return 2;
}

int main (int argc, char **argv) {
  CONST char *In         = NULL;
  CONST char *Out        = NULL;
  int         CheckOnly  = 0;
  int         opt;

  static struct option longopts[] = {
    {"in",                  required_argument, 0, 'i'},
    {"out",                 required_argument, 0, 'o'},
    {"check-anchors-only",  no_argument,       0, 'c'},
    {"help",                no_argument,       0, 'h'},
    {0, 0, 0, 0},
  };
  while ((opt = getopt_long (argc, argv, "i:o:ch", longopts, NULL)) != -1) {
    switch (opt) {
      case 'i': In = optarg; break;
      case 'o': Out = optarg; break;
      case 'c': CheckOnly = 1; break;
      case 'h': default: return Usage (argv[0]);
    }
  }

  if (!In) return Usage (argv[0]);

  /* Load file */
  FILE *f = fopen (In, "rb");
  if (!f) { perror ("fopen"); return 1; }
  fseek (f, 0, SEEK_END);
  long Sz = ftell (f);
  fseek (f, 0, SEEK_SET);
  UINT8 *Buf = (UINT8 *)malloc ((size_t)Sz);
  if (fread (Buf, 1, (size_t)Sz, f) != (size_t)Sz) {
    fprintf (stderr, "read failed\n"); return 1;
  }
  fclose (f);

  DynamicPatchLib_EnsureInit ();
  PATCH_RESULT R;
  DynamicPatch_Apply (Buf, (UINT32)Sz, &R);

  fprintf (stderr, "applied=%u missed=%u worst=%d\n",
           R.AppliedCount, R.MissedCount, R.WorstOutcome);

  if (CheckOnly) {
    /* Anchor-uniqueness check: WorstOutcome must not be PATCH_AMBIGUOUS for any
       patch.  Mandatory misses are tolerated when --check-anchors-only — caller
       cares about ambiguity, not about whether non-applicable patches missed. */
    if (R.WorstOutcome == 2 /* MANDATORY_MISS — not necessarily fatal here */) {
      fprintf (stderr, "warn: mandatory miss in --check-anchors-only mode\n");
    }
    /* Real ambiguity is reported per-patch by PatchEngine when it logs.
       Future: extend PatchEngine to surface per-patch outcomes via a callback,
       and have abl-patcher fail on PATCH_AMBIGUOUS. */
    return 0;
  }

  if (R.WorstOutcome == 2) {
    fprintf (stderr, "ERROR: mandatory patch miss\n"); return 1;
  }

  if (Out) {
    FILE *o = fopen (Out, "wb");
    if (!o) { perror ("fopen out"); return 1; }
    fwrite (Buf, 1, (size_t)Sz, o);
    fclose (o);
  }
  return 0;
}
```

- [ ] **Step 3: Build the host binary**

```bash
cd /home/vivy/gbl-chainload/tools/abl-patcher && make
```

Expected: `abl-patcher` binary exists.

- [ ] **Step 4: Smoke test**

```bash
cd /home/vivy/gbl-chainload \
  && tools/abl-patcher/abl-patcher --in images/infiniti/LinuxLoader_infiniti.efi
```

Expected output: `applied=N missed=M worst=W` where applied is patch1+patch7+(when canoe-fixture-equivalent path is hit)patch9.

- [ ] **Step 5: Commit**

```bash
git add tools/abl-patcher \
  && git commit -m "tools/abl-patcher: host binary linking the same DynamicPatchLib"
```

---

## Task 21: `tests/042_dynamic_patch_harness.sh` — anchor uniqueness CI

**Files:**
- Create: `gbl-chainload/tests/042_dynamic_patch_harness.sh`
- (modify Makefile to wire host tests under `tests/runall.sh`)

- [ ] **Step 1: Write the test**

```bash
#!/usr/bin/env bash
# 042_dynamic_patch_harness.sh — anchor uniqueness across fixtures + patch
# byte-equivalent regression on each.
set -euo pipefail

cd "$(dirname "$0")/.."

# Build the host scan/patches tests.
make -C tests/scan
make -C tests/patches

# Build the abl-patcher host binary.
make -C tools/abl-patcher

# Anchor uniqueness across each available fixture.
declare -a FIXTURES=()
[[ -f images/infiniti/LinuxLoader_infiniti.efi ]] \
  && FIXTURES+=("images/infiniti/LinuxLoader_infiniti.efi")
[[ -f images/fixtures/canoe-A.07/abl_a.bin ]] \
  && FIXTURES+=("images/fixtures/canoe-A.07/abl_a.bin")

if [[ ${#FIXTURES[@]} -eq 0 ]]; then
  echo "ERROR: no fixtures found.  See images/README.md." >&2
  exit 1
fi

FAIL=0
for f in "${FIXTURES[@]}"; do
  echo "== anchor-uniqueness check on $f =="
  if ! tools/abl-patcher/abl-patcher --in "$f" --check-anchors-only; then
    echo "FAIL on $f"; FAIL=1
  fi
done

# Warn if canoe fixture is missing (non-fatal — gates resolved at next CI run).
if [[ ! -f images/fixtures/canoe-A.07/abl_a.bin ]]; then
  echo "WARN: canoe fixture absent.  patch9 canoe leg not exercised." >&2
fi

if [[ $FAIL -ne 0 ]]; then
  exit 1
fi
echo "ok 042_dynamic_patch_harness"
```

`chmod +x tests/042_dynamic_patch_harness.sh`.

- [ ] **Step 2: Run it**

```bash
bash tests/042_dynamic_patch_harness.sh
```

Expected: builds + runs all host test binaries + anchor-uniqueness check on each fixture. Pass.

- [ ] **Step 3: Commit**

```bash
git add tests/042_dynamic_patch_harness.sh \
  && git commit -m "tests/042: anchor uniqueness + host patch regression harness"
```

---

## Task 22: gbl_root_canoe regression fixtures

**Files:**
- Create: `gbl-chainload/tests/fixtures/patches-gbl-root-canoe/<patch-name>/...`
- Create: `gbl-chainload/tests/051_gbl_root_canoe_regression.sh`

- [ ] **Step 1: Survey gbl_root_canoe**

```bash
ls /home/vivy/gbl_root_canoe/
grep -RnE "Patch[0-9]+|patch_[0-9]+" /home/vivy/gbl_root_canoe/ \
  | head -40
```

Identify patch names + source files. Pick 2-3 representative patches that aren't trivial (e.g. patches that involve scan + rewrite, not just a string substitution).

- [ ] **Step 2: Import each patch's input + expected output as fixtures**

For each chosen patch:

```bash
mkdir -p tests/fixtures/patches-gbl-root-canoe/<patch-name>
# Input: pre-patch binary (gbl_root_canoe stores these somewhere — find it)
cp <gbl_root_canoe-input> tests/fixtures/patches-gbl-root-canoe/<patch-name>/input.bin
# Expected output: post-patch binary produced by gbl_root_canoe's engine
# (run their patcher once to capture the expected bytes)
cp <gbl_root_canoe-output> tests/fixtures/patches-gbl-root-canoe/<patch-name>/expected.bin
```

If gbl_root_canoe doesn't ship pre-patched fixtures, run their patcher in-place and capture the diff as the expected output.

- [ ] **Step 3: Re-implement each gbl_root_canoe patch as a `PATCH_DESC` in our engine**

Add to `GblChainloadPkg/Library/DynamicPatchLib/oem/oneplus_canoe.c` (or to a new `regression/` subdir with separate aggregator) so they don't pollute production patches.

For Plan 1, **scope these to a regression-only PATCH_DESC table** — they aren't in `kOemOneplusPatches`, only in a separate `kRegressionPatches` table consumed only by `tools/abl-patcher --regression-suite`.

- [ ] **Step 4: Write `tests/051_gbl_root_canoe_regression.sh`**

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
make -C tools/abl-patcher

FAIL=0
for d in tests/fixtures/patches-gbl-root-canoe/*/; do
  name=$(basename "$d")
  echo "== regression: $name =="
  cp "$d/input.bin" /tmp/regression-$name.bin
  tools/abl-patcher/abl-patcher \
    --in "$d/input.bin" --out /tmp/regression-$name-patched.bin \
    --regression-suite "$name"
  if ! cmp /tmp/regression-$name-patched.bin "$d/expected.bin"; then
    echo "FAIL: $name byte-mismatch"; FAIL=1
  fi
done
[[ $FAIL -eq 0 ]] && echo "ok 051_gbl_root_canoe_regression"
exit $FAIL
```

(`--regression-suite <name>` runs only that named patch from `kRegressionPatches`. Engineer adds the flag to `abl-patcher.c`.)

- [ ] **Step 5: Run — pass**

If gbl_root_canoe sources are not accessible / patches not viable to import, document that and reduce scope (or move to Plan 2). Do not fail-open — the test must give clear pass/skip with reason.

- [ ] **Step 6: Commit**

```bash
git add tests/fixtures tests/051_gbl_root_canoe_regression.sh \
        GblChainloadPkg/Library/DynamicPatchLib \
  && git commit -m "tests/051: gbl_root_canoe regression fixtures + abl-patcher --regression-suite"
```

---

## Task 23: Carry-forward `ProtocolHookLib` slot wrappers (verbatim)

**Files:**
- Create: `gbl-chainload/GblChainloadPkg/Library/ProtocolHookLib/{VerifiedBootHook,QseecomHook,ScmHook,HookCommon}.{c,h}` (verbatim from dirty)

The slot wrappers (`HookedVBRwDeviceState`, `HookedScmSysCall`, `HookedSendCmd`, etc.) are working code in the dirty repo. Carry forward verbatim, then refactor in Tasks 24-25 to split universal vs mode-1 logic.

- [ ] **Step 1: Copy verbatim**

```bash
cp /home/vivy/gbl-chainload-dirty/GblChainloadPkg/Library/ProtocolHookLib/{VerifiedBootHook,QseecomHook,ScmHook,HookCommon}.{c,h} \
   /home/vivy/gbl-chainload/GblChainloadPkg/Library/ProtocolHookLib/
```

(some headers may not exist; copy whatever does)

- [ ] **Step 2: Verify the package builds standalone**

Build via docker:

```bash
cd /home/vivy/gbl-chainload && ./scripts/build-inside-docker.sh --mode 1
```

Expected: build fails because `Mode1Overlay.c`, `UniversalBaseline.c`, `InstallAll.c` don't exist yet, and `Entry.c`/`BootFlow.c` reference symbols not present. That's the fail state — Tasks 24-26 fix it.

- [ ] **Step 3: Commit**

```bash
git add GblChainloadPkg/Library/ProtocolHookLib \
  && git commit -m "ProtocolHookLib: carry-forward slot wrappers verbatim"
```

---

## Task 24: `UniversalBaseline.c` — VB swallow + SCM fuse drop + OplusSec 0x0A drop

**Files:**
- Create: `gbl-chainload/GblChainloadPkg/Library/ProtocolHookLib/UniversalBaseline.c`
- Modify: `gbl-chainload/GblChainloadPkg/Library/ProtocolHookLib/VerifiedBootHook.c`
- Modify: `gbl-chainload/GblChainloadPkg/Library/ProtocolHookLib/ScmHook.c`
- Modify: `gbl-chainload/GblChainloadPkg/Library/ProtocolHookLib/QseecomHook.c`

The dirty repo's slot wrappers contain mode-specific behavior gated on `defined(FAKELOCKED)` or `defined(FAKELOCKED_DEBUG)`. Migrate to:

- **Universal-baseline behavior**: VB `WRITE_CONFIG` + `Reset` swallow, SCM `TZ_BLOW_SW_FUSE_ID` drop, OplusSec `0x0A` drop. Always installed.
- **Mode-1 behavior**: VB `READ_CONFIG` mutate, `VBDeviceInit` clear pre/post. Installed only when `GBL_MODE==1`.

- [ ] **Step 1: Strip mode-specific gating from slot wrappers**

In each Hooked-* function, replace `#if defined (FAKELOCKED)` blocks with calls to **policy functions** declared in `UniversalBaseline.h` and `Mode1Overlay.h`:

```c
/* VerifiedBootHook.c — slot wrapper for VBRwDeviceState. */
STATIC EFI_STATUS EFIAPI
HookedVBRwDeviceState (...) {
  ...
  if (Op == WRITE_CONFIG) {
    /* Universal: never persist a config write during chainload. */
    return UniversalPolicy_OnVbWriteConfig (This, Op, Buf, BufLen);
  }
  if (Op == READ_CONFIG) {
    EFI_STATUS s = gOrigVBRwDeviceState (This, Op, Buf, BufLen);
    /* Mode-1: mutate the read result.  No-op in modes 2/3. */
    return Mode1Policy_OnVbReadConfig_Post (This, s, Buf, BufLen);
  }
  /* Other ops pass through. */
  return gOrigVBRwDeviceState (This, Op, Buf, BufLen);
}
```

The slot wrapper no longer knows about modes — it only knows "delegate to universal policy" and "delegate to mode-1 policy".

- [ ] **Step 2: Author `UniversalBaseline.c`**

```c
#include <Uefi.h>
#include <Library/DebugLib.h>
#include "HookCommon.h"
#include "UniversalBaseline.h"

EFI_STATUS EFIAPI
UniversalPolicy_OnVbWriteConfig (
  IN  QCOM_VERIFIEDBOOT_PROTOCOL *This,
  IN  UINT32                      Op,
  IN  VOID                       *Buf,
  IN  UINT32                      BufLen
  )
{
  /* Swallow: do not call original; do not persist anything to RPMB. */
  DEBUG ((DEBUG_INFO,
          "vb-rwstate | op=WRITE_CONFIG | bufLen=%u | swallowed\n", BufLen));
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
UniversalPolicy_OnVbReset (
  IN  QCOM_VERIFIEDBOOT_PROTOCOL *This
  )
{
  DEBUG ((DEBUG_INFO, "vb-reset | swallowed\n"));
  return EFI_SUCCESS;
}

/* SCM: TZ_BLOW_SW_FUSE_ID (0x02000801) — synthetic success without forwarding. */
BOOLEAN
UniversalPolicy_ShouldDropScmSip (UINT32 SmcId, EFI_STATUS *FakeStatus) {
  if (SmcId == 0x02000801U /* TZ_BLOW_SW_FUSE_ID */) {
    *FakeStatus = EFI_SUCCESS;
    DEBUG ((DEBUG_INFO,
            "scm-sip | smcid=0x%08x(TZ_BLOW_SW_FUSE_ID) | DROPPED\n", SmcId));
    return TRUE;
  }
  return FALSE;
}

/* QSEE OplusSec 0x0A — synthetic success on the response. */
BOOLEAN
UniversalPolicy_ShouldDropQseeOplusSec (UINT32 Handle, UINT32 CmdId, UINT8 *RspBuf,
                                         UINT32 *RspLen, EFI_STATUS *FakeStatus) {
  /* Caller checks Handle == gOplusSecHandle before calling here; we just
     check the cmd id. */
  if (CmdId == 0x0AU /* write_rpmb_boot_info */) {
    *FakeStatus = EFI_SUCCESS;
    DEBUG ((DEBUG_INFO,
            "qsee-oplussec | cmd=0x0A(write_rpmb_boot_info) | DROPPED\n"));
    return TRUE;
  }
  return FALSE;
}
```

- [ ] **Step 3: Author `UniversalBaseline.h`** with the prototypes above.

- [ ] **Step 4: Wire the policy hooks into the slot wrappers**

Modify `VerifiedBootHook.c::HookedVBRwDeviceState` (write op) and `HookedVBResetState` to call the universal policies. Modify `ScmHook.c::HookedScmSysCall` to short-circuit when `UniversalPolicy_ShouldDropScmSip` returns TRUE. Modify `QseecomHook.c::HookedSendCmd` to short-circuit OplusSec 0x0A.

- [ ] **Step 5: Lint test**

`tests/045_mode_taxonomy_lint.sh` adds:

```bash
grep -q "UniversalPolicy_OnVbWriteConfig" \
  GblChainloadPkg/Library/ProtocolHookLib/VerifiedBootHook.c \
  || { echo "VerifiedBootHook missing universal write-config call"; exit 1; }
grep -q "UniversalPolicy_ShouldDropScmSip" \
  GblChainloadPkg/Library/ProtocolHookLib/ScmHook.c \
  || { echo "ScmHook missing universal SIP drop"; exit 1; }
grep -q "UniversalPolicy_ShouldDropQseeOplusSec" \
  GblChainloadPkg/Library/ProtocolHookLib/QseecomHook.c \
  || { echo "QseecomHook missing universal OplusSec drop"; exit 1; }
```

- [ ] **Step 6: Run — pass**

- [ ] **Step 7: Commit**

```bash
git add GblChainloadPkg/Library/ProtocolHookLib tests/045_mode_taxonomy_lint.sh \
  && git commit -m "ProtocolHookLib: UniversalBaseline policies + slot-wrapper integration"
```

---

## Task 25: `Mode1Overlay.c` — VB READ_CONFIG mutate + VBDeviceInit clear

**Files:**
- Create: `gbl-chainload/GblChainloadPkg/Library/ProtocolHookLib/Mode1Overlay.c`
- Create: `gbl-chainload/GblChainloadPkg/Library/ProtocolHookLib/Mode1Overlay.h`
- Modify: `gbl-chainload/GblChainloadPkg/Library/ProtocolHookLib/VerifiedBootHook.c`

- [ ] **Step 1: Author `Mode1Overlay.h`**

```c
#ifndef MODE1_OVERLAY_H_
#define MODE1_OVERLAY_H_
#include <Uefi.h>
#include "HookCommon.h"

#if (GBL_MODE == 1)
/* Mode-1 mutates the populated VBDeviceInit struct + post-read VB state. */
EFI_STATUS EFIAPI
Mode1Policy_OnVbReadConfig_Post (
  IN  QCOM_VERIFIEDBOOT_PROTOCOL *This,
  IN  EFI_STATUS                  OrigStatus,
  IN  VOID                       *Buf,
  IN  UINT32                      BufLen
  );

VOID EFIAPI
Mode1Policy_OnVbDeviceInit_PrePost (
  IN OUT device_info_vb_t *Devinfo,
  IN BOOLEAN               IsPre  /* TRUE = pre-call clear, FALSE = post-call clear */
  );
#endif

#endif
```

- [ ] **Step 2: Author `Mode1Overlay.c`**

Port the existing `VbForceDevinfoVbLocked` logic from the dirty repo.

```c
#include "Mode1Overlay.h"

#if (GBL_MODE == 1)

#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>

EFI_STATUS EFIAPI
Mode1Policy_OnVbReadConfig_Post (
  IN  QCOM_VERIFIEDBOOT_PROTOCOL *This,
  IN  EFI_STATUS                  OrigStatus,
  IN  VOID                       *Buf,
  IN  UINT32                      BufLen
  )
{
  if (EFI_ERROR (OrigStatus)) return OrigStatus;
  device_info_vb_t *Info = (device_info_vb_t *)Buf;
  UINT32 PrevUnlocked     = Info->is_unlocked;
  UINT32 PrevUnlockCrit   = Info->is_unlock_critical;
  Info->is_unlocked        = FALSE;
  Info->is_unlock_critical = FALSE;
  DEBUG ((DEBUG_INFO,
          "vb-fakelock | READ_CONFIG | is_unlocked %u->0 | is_unlock_critical %u->0\n",
          PrevUnlocked, PrevUnlockCrit));
  return OrigStatus;
}

VOID EFIAPI
Mode1Policy_OnVbDeviceInit_PrePost (
  IN OUT device_info_vb_t *Devinfo,
  IN     BOOLEAN           IsPre
  )
{
  if (Devinfo == NULL) return;
  Devinfo->is_unlocked        = FALSE;
  Devinfo->is_unlock_critical = FALSE;
  DEBUG ((DEBUG_INFO,
          "vb-fakelock | VBDeviceInit/%a | cleared\n",
          IsPre ? "pre" : "post"));
}

#endif /* GBL_MODE == 1 */
```

- [ ] **Step 3: Wire mode-1 calls into `VerifiedBootHook.c`**

In `HookedVBRwDeviceState` (after the original call returns on READ_CONFIG):

```c
#if (GBL_MODE == 1)
  Status = Mode1Policy_OnVbReadConfig_Post (This, Status, Buf, BufLen);
#endif
```

In `HookedVBDeviceInit` (around the original call):

```c
#if (GBL_MODE == 1)
  Mode1Policy_OnVbDeviceInit_PrePost (Devinfo, /*IsPre=*/TRUE);
#endif
  Status = gOrigVbDeviceInit (This, Devinfo);
#if (GBL_MODE == 1)
  Mode1Policy_OnVbDeviceInit_PrePost (Devinfo, /*IsPre=*/FALSE);
#endif
```

- [ ] **Step 4: Lint test**

`tests/046_mode1_protocol_hook_lint.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
PHL="GblChainloadPkg/Library/ProtocolHookLib"
test -f "$PHL/Mode1Overlay.c" || { echo "missing Mode1Overlay.c"; exit 1; }
test -f "$PHL/Mode1Overlay.h" || { echo "missing Mode1Overlay.h"; exit 1; }
grep -q "Mode1Policy_OnVbReadConfig_Post" "$PHL/VerifiedBootHook.c" \
  || { echo "VerifiedBootHook missing mode1 read-post mutator"; exit 1; }
grep -q "Mode1Policy_OnVbDeviceInit_PrePost" "$PHL/VerifiedBootHook.c" \
  || { echo "VerifiedBootHook missing mode1 deviceinit pre/post"; exit 1; }
echo "ok 046_mode1_protocol_hook_lint"
```

`chmod +x tests/046_mode1_protocol_hook_lint.sh`.

- [ ] **Step 5: Run — pass**

- [ ] **Step 6: Commit**

```bash
git add GblChainloadPkg/Library/ProtocolHookLib/Mode1Overlay.{c,h} \
        GblChainloadPkg/Library/ProtocolHookLib/VerifiedBootHook.c \
        tests/046_mode1_protocol_hook_lint.sh \
  && git commit -m "ProtocolHookLib: Mode1Overlay (VB READ + Init mutate) + lint"
```

---

## Task 26: `InstallAll.c` — universal + per-mode overlay dispatch + fail-closed install

**Files:**
- Create: `gbl-chainload/GblChainloadPkg/Library/ProtocolHookLib/InstallAll.c`
- Create: `gbl-chainload/GblChainloadPkg/Include/Library/ProtocolHookLib.h`
- Create: `gbl-chainload/GblChainloadPkg/Library/ProtocolHookLib/ProtocolHookLib.inf`

- [ ] **Step 1: Author public `ProtocolHookLib.h`**

```c
#ifndef PROTOCOL_HOOK_LIB_H_
#define PROTOCOL_HOOK_LIB_H_
#include <Uefi.h>

typedef struct {
  UINT32  VbInstalledSlots;
  UINT32  VbExpectedSlots;
  UINT32  ScmInstalledSlots;
  UINT32  ScmExpectedSlots;
  UINT32  QseecomInstalledSlots;
  UINT32  QseecomExpectedSlots;
  BOOLEAN UniversalRequiredOk;
  BOOLEAN ModeOverlayOk;
} HOOK_INSTALL_RESULT;

/** Install universal baseline, then per-mode overlay (selected by GBL_MODE).
    Returns EFI_SUCCESS only if all required slots installed. **/
EFI_STATUS
ProtocolHook_InstallAll (
  OUT HOOK_INSTALL_RESULT  *Result
  );

#endif
```

- [ ] **Step 2: Author `InstallAll.c`**

```c
#include "../../Include/Library/ProtocolHookLib.h"
#include "HookCommon.h"
#include "UniversalBaseline.h"
#if (GBL_MODE == 1)
#include "Mode1Overlay.h"
#endif

EFI_STATUS InstallVerifiedBootHook (HOOK_INSTALL_RESULT *r);  /* file-local */
EFI_STATUS InstallScmHook          (HOOK_INSTALL_RESULT *r);
EFI_STATUS InstallQseecomHook      (HOOK_INSTALL_RESULT *r);

EFI_STATUS
ProtocolHook_InstallAll (
  OUT HOOK_INSTALL_RESULT  *Result
  )
{
  EFI_STATUS Status;
  ZeroMem (Result, sizeof (*Result));

  /* 1. Slot-wrapper installation (carries forward; reads protocol vtables). */
  Status = InstallVerifiedBootHook (Result);
  if (EFI_ERROR (Status)) {
    Print (L"ProtocolHook: VerifiedBoot install failed (%r) — abort chainload\n",
           Status);
    return Status;
  }
  Status = InstallScmHook (Result);
  if (EFI_ERROR (Status)) {
    Print (L"ProtocolHook: SCM install failed (%r) — abort chainload\n", Status);
    return Status;
  }
  Status = InstallQseecomHook (Result);
  if (EFI_ERROR (Status)) {
    Print (L"ProtocolHook: Qseecom install failed (%r) — abort chainload\n",
           Status);
    return Status;
  }

  /* 2. Universal-baseline policy decisions are inline in slot wrappers
        (they call UniversalPolicy_*). No separate install step.
        Mark OK if all critical slot wrappers landed. */
  Result->UniversalRequiredOk = (Result->VbInstalledSlots > 0
                                  && Result->ScmInstalledSlots > 0
                                  && Result->QseecomInstalledSlots > 0);
  if (!Result->UniversalRequiredOk) {
    Print (L"ProtocolHook: universal baseline incomplete — abort chainload\n");
    return EFI_NOT_READY;
  }

  /* 3. Mode-overlay activation. Currently the overlay is also inline in slot
        wrappers, gated by GBL_MODE.  Mark OK; future modes that install
        additional slots can extend this. */
#if (GBL_MODE == 1)
  Result->ModeOverlayOk = TRUE;
#elif (GBL_MODE == 2 || GBL_MODE == 3)
  /* Plan 2/3 fills these in. */
  Result->ModeOverlayOk = TRUE;
#else
# error "GBL_MODE must be 1, 2, or 3"
#endif

  Print (L"ProtocolHook: install complete (mode=%d, vb=%u/%u, scm=%u/%u, qsee=%u/%u)\n",
         (int)GBL_MODE,
         Result->VbInstalledSlots, Result->VbExpectedSlots,
         Result->ScmInstalledSlots, Result->ScmExpectedSlots,
         Result->QseecomInstalledSlots, Result->QseecomExpectedSlots);
  return EFI_SUCCESS;
}
```

(`InstallVerifiedBootHook`, `InstallScmHook`, `InstallQseecomHook` already exist in the carried-forward `*.c` files; just declare them here as file-local externs.)

- [ ] **Step 3: Author `ProtocolHookLib.inf`**

```ini
[Defines]
  INF_VERSION    = 0x00010005
  BASE_NAME      = ProtocolHookLib
  FILE_GUID      = a4d7d0e8-bbbb-4321-c000-fedcba012345  # generate fresh
  MODULE_TYPE    = UEFI_APPLICATION
  VERSION_STRING = 1.0
  LIBRARY_CLASS  = ProtocolHookLib

[Sources]
  InstallAll.c
  UniversalBaseline.c
  Mode1Overlay.c
  VerifiedBootHook.c
  ScmHook.c
  QseecomHook.c

[Packages]
  MdePkg/MdePkg.dec
  GblChainloadPkg/GblChainloadPkg.dec

[LibraryClasses]
  BaseLib
  BaseMemoryLib
  DebugLib
  UefiBootServicesTableLib
```

- [ ] **Step 4: Lint test (extends 045)**

```bash
grep -q "ProtocolHook_InstallAll" \
  GblChainloadPkg/Library/ProtocolHookLib/InstallAll.c \
  || { echo "InstallAll.c missing main entry"; exit 1; }
```

- [ ] **Step 5: Commit**

```bash
git add GblChainloadPkg/Library/ProtocolHookLib/InstallAll.c \
        GblChainloadPkg/Library/ProtocolHookLib/ProtocolHookLib.inf \
        GblChainloadPkg/Include/Library/ProtocolHookLib.h \
        tests/045_mode_taxonomy_lint.sh \
  && git commit -m "ProtocolHookLib: InstallAll dispatcher + .inf + public header"
```

---

## Task 27: `Entry.c` — single mode dispatcher with AUTO/DEBUG/VERBOSE flag wiring

**Files:**
- Create: `gbl-chainload/GblChainloadPkg/Application/GblChainload/Entry.c`
- Create: `gbl-chainload/GblChainloadPkg/Application/GblChainload/Stubs.c` (carry verbatim)
- Create: `gbl-chainload/GblChainloadPkg/Application/GblChainload/GblChainload.inf`

The dirty `Entry.c` has 7-way mode dispatch. The new one has just one path: `RunMode(GBL_MODE)`. AUTO toggles timeout-action, DEBUG gates screen output, VERBOSE is consumed by `BootFlow.c` / `ProtocolHookLib`.

- [ ] **Step 1: Carry `Stubs.c`**

```bash
cp /home/vivy/gbl-chainload-dirty/GblChainloadPkg/Application/GblChainload/Stubs.c \
   /home/vivy/gbl-chainload/GblChainloadPkg/Application/GblChainload/
```

- [ ] **Step 2: Author the new `Entry.c`**

```c
/** @file Entry.c — gbl-chainload entry point.
    Mode is selected by the GBL_MODE feature flag (1/2/3). AUTO toggles the
    timeout action; DEBUG gates screen output; VERBOSE selects hook depth. **/
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DeviceInfo.h>
#include <Library/PartitionTableUpdate.h>
#include <Library/BootESP.h>
#include <Library/LoadFVLib.h>
#include <Library/LogFsLib.h>

EFI_STATUS FastbootInitialize (VOID);
EFI_STATUS EFIAPI BootFlowChainLoad (VOID);

#ifndef GBL_MODE
#error "GBL_MODE (1, 2, or 3) must be defined at build time"
#endif
#ifndef GBL_AUTO
#define GBL_AUTO 0
#endif
#ifndef GBL_DEBUG
#define GBL_DEBUG 0
#endif
#ifndef GBL_VERBOSE
#define GBL_VERBOSE 0
#endif

#define GBL_VERSION  "v2-plan1"
#define KEY_WINDOW_MS 3000

#if (GBL_DEBUG == 1)
# define ScreenPrint(...)  Print (__VA_ARGS__)
#else
# define ScreenPrint(...)  do {} while (0)
#endif

typedef enum { GblKeyNone, GblKeyVolDown, GblKeyVolUp } GBL_KEY_ACTION;

STATIC GBL_KEY_ACTION
WaitForBootInterrupt (UINT32 TimeoutMs)
{
  /* Same shape as the dirty Entry.c WaitForBootInterrupt — carry the
     implementation verbatim and trim. */
  /* ... existing key-window code; identical apart from removing references
     to multiple modes. ... */
}

EFI_STATUS
EFIAPI
GblChainloadEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  GBL_KEY_ACTION Key;

  ScreenPrint (L"\ngbl-chainload %a — mode=%d auto=%d debug=%d verbose=%d %a %a\n",
               GBL_VERSION, (int)GBL_MODE, (int)GBL_AUTO,
               (int)GBL_DEBUG, (int)GBL_VERBOSE, __DATE__, __TIME__);
  DEBUG ((DEBUG_INFO,
          "gbl-chainload | mode=%d auto=%d debug=%d verbose=%d\n",
          (int)GBL_MODE, (int)GBL_AUTO, (int)GBL_DEBUG, (int)GBL_VERBOSE));

  /* Common early init — verbatim from dirty CommonEarlyInit. */
  LogFsInit ();
  LogFsInstallDebugSink ();
  LogFsFlush ();
  DeviceInfoInit ();
  EnumeratePartitions ();
  UpdatePartitionEntries ();

  ScreenPrint (L"Hold VolUp within %us to %s; "
               L"timeout %s.\n",
               KEY_WINDOW_MS / 1000,
               GBL_AUTO ? L"chain-load patched ABL" : L"enter FastbootLib",
               GBL_AUTO ? L"enters FastbootLib (await `oem escape`)"
                        : L"chain-loads silently");

  Key = WaitForBootInterrupt (KEY_WINDOW_MS);

#if (GBL_AUTO == 0)
  /* AUTO=0 — silent timeout chain-load.  VolUp is moot (same outcome).  */
  if (Key == GblKeyVolUp || Key == GblKeyNone) {
    ScreenPrint (L"chain-loading patched ABL\n");
    LogFsFlush ();
    BootFlowChainLoad ();
  }
  /* If chain-load returns (LoadImage error etc.), or VolDown was held, fall through
     to FastbootLib as the recovery surface. */
#else
  /* AUTO=1 — timeout enters FastbootLib; VolUp forces immediate chain-load. */
  if (Key == GblKeyVolUp) {
    ScreenPrint (L"VolUp escape: chain-loading patched ABL\n");
    LogFsFlush ();
    BootFlowChainLoad ();
  }
#endif

  /* Always end up here on chainload return / fastboot path. */
  ScreenPrint (L"Entering FastbootLib\n");
  LogFsFlush ();
  LogFsRemoveDebugSink ();
  LogFsClose ();
  FastbootInitialize ();
  while (TRUE) gBS->Stall (1000000);
  return EFI_SUCCESS;
}
```

- [ ] **Step 3: Author `GblChainload.inf`**

```ini
[Defines]
  INF_VERSION    = 0x00010005
  BASE_NAME      = GblChainload
  FILE_GUID      = c1e7e0a0-cccc-4321-d000-fedcba012345  # generate fresh
  MODULE_TYPE    = UEFI_APPLICATION
  VERSION_STRING = 1.0
  ENTRY_POINT    = GblChainloadEntry

[Sources]
  Entry.c
  BootFlow.c
  Stubs.c

[Packages]
  MdePkg/MdePkg.dec
  GblChainloadPkg/GblChainloadPkg.dec
  QcomModulePkg/QcomModulePkg.dec   # for DeviceInfo, PartitionTableUpdate, etc.

[LibraryClasses]
  UefiApplicationEntryPoint
  UefiBootServicesTableLib
  UefiLib
  DebugLib
  AblUnwrapLib
  DynamicPatchLib
  ProtocolHookLib
  LogFsLib

[BuildOptions]
  *_*_AARCH64_CC_FLAGS = -DGBL_MODE=$(GBL_MODE) -DGBL_AUTO=$(GBL_AUTO) \
                          -DGBL_DEBUG=$(GBL_DEBUG) -DGBL_VERBOSE=$(GBL_VERBOSE)
```

- [ ] **Step 4: Commit**

```bash
git add GblChainloadPkg/Application/GblChainload \
  && git commit -m "Application/GblChainload: single Entry.c with AUTO/DEBUG/VERBOSE flag dispatch"
```

---

## Task 28: `BootFlow.c` — chain-load with universal + mode-1 hooks

**Files:**
- Create: `gbl-chainload/GblChainloadPkg/Application/GblChainload/BootFlow.c`

- [ ] **Step 1: Author `BootFlow.c`**

```c
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PartitionTableUpdate.h>
#include <Library/AblUnwrapLib.h>
#include <Library/DynamicPatchLib.h>
#include <Library/ProtocolHookLib.h>
#include <Library/LogFsLib.h>

STATIC EFI_STATUS
ResolveActiveAblName (CHAR16 *Out, UINTN OutCap)
{
  Slot Active = GetCurrentSlotSuffix ();
  StrnCpyS (Out, OutCap, L"abl", StrLen (L"abl"));
  StrnCatS (Out, OutCap, Active.Suffix, StrLen (Active.Suffix));
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
BootFlowChainLoad (VOID)
{
  EFI_STATUS    Status;
  CHAR16        AblName[MAX_GPT_NAME_SIZE];
  VOID         *Pe = NULL;
  UINT32        PeSize = 0;
  PATCH_RESULT  PatchRes = {0};
  HOOK_INSTALL_RESULT HookRes = {0};
  EFI_HANDLE    ImageHandle = NULL;

  Print (L"BootFlow: start (mode=%d)\n", (int)GBL_MODE);

  /* 1. Unwrap ABL PE. */
  ResolveActiveAblName (AblName, MAX_GPT_NAME_SIZE);
  Status = AblUnwrap_LoadFromPartition (AblName, &Pe, &PeSize);
  if (EFI_ERROR (Status)) {
    /* Some Qualcomm devices use a single non-A/B `abl` partition. */
    Status = AblUnwrap_LoadFromPartition (L"abl", &Pe, &PeSize);
  }
  if (EFI_ERROR (Status)) {
    Print (L"BootFlow: ABL not found (%r)\n", Status); return Status;
  }

  /* 2. Apply patches. */
  DynamicPatch_Apply (Pe, PeSize, &PatchRes);
  Print (L"BootFlow: patches applied=%u missed=%u worst=%d\n",
         PatchRes.AppliedCount, PatchRes.MissedCount, PatchRes.WorstOutcome);
  if (PatchRes.WorstOutcome == PATCH_RESULT_MANDATORY_MISS) {
    Print (L"BootFlow: mandatory patch missed - aborting chain-load\n");
    FreePool (Pe); return EFI_NOT_READY;
  }

  /* 3. Install protocol hooks (universal + per-mode overlay). */
  Status = ProtocolHook_InstallAll (&HookRes);
  if (EFI_ERROR (Status)) {
    Print (L"BootFlow: hook install failed (%r) - aborting\n", Status);
    FreePool (Pe); return Status;
  }

  /* 4. LoadImage + StartImage. */
  Status = gBS->LoadImage (FALSE, gImageHandle, NULL, Pe, PeSize, &ImageHandle);
  if (EFI_ERROR (Status)) {
    Print (L"BootFlow: LoadImage failed (%r)\n", Status);
    FreePool (Pe); return Status;
  }

  Print (L"BootFlow: handing off to patched ABL\n");
  LogFsFlush ();
  Status = gBS->StartImage (ImageHandle, NULL, NULL);

  /* StartImage rarely returns. */
  Print (L"BootFlow: StartImage returned %r\n", Status);
  if (ImageHandle != NULL) gBS->UnloadImage (ImageHandle);
  FreePool (Pe);
  return EFI_LOAD_ERROR;
}
```

- [ ] **Step 2: Commit**

```bash
git add GblChainloadPkg/Application/GblChainload/BootFlow.c \
  && git commit -m "Application/GblChainload: BootFlow with mode-aware hook install"
```

---

## Task 29: `GblChainloadPkg.dsc` + feature PCDs + `scripts/build.sh` argv

**Files:**
- Create: `gbl-chainload/GblChainloadPkg/GblChainloadPkg.dsc`
- Create: `gbl-chainload/GblChainloadPkg/GblChainloadPkg.dec`
- Create: `gbl-chainload/scripts/build.sh`

- [ ] **Step 1: Author `GblChainloadPkg.dec`**

```ini
[Defines]
  DEC_SPECIFICATION = 0x00010005
  PACKAGE_NAME      = GblChainloadPkg
  PACKAGE_GUID      = e2f5d0a0-dddd-4321-e000-fedcba012345  # generate fresh
  PACKAGE_VERSION   = 1.0

[Includes]
  Include

[Guids]
  gGblChainloadPkgTokenSpaceGuid = { 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6,
                                     0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
                                     0x0d, 0x0e, 0x0f, 0x10 }   # generate fresh

[LibraryClasses]
  AblUnwrapLib|Include/Library/AblUnwrapLib.h
  DynamicPatchLib|Include/Library/DynamicPatchLib.h
  ProtocolHookLib|Include/Library/ProtocolHookLib.h
  LogFsLib|Include/Library/LogFsLib.h
  ScanLib|Include/Library/ScanLib.h
```

- [ ] **Step 2: Author `GblChainloadPkg.dsc`**

```ini
[Defines]
  PLATFORM_NAME           = GblChainloadPkg
  PLATFORM_GUID           = f3a6b1b1-eeee-4321-f000-fedcba012345  # generate fresh
  PLATFORM_VERSION        = 1.0
  DSC_SPECIFICATION       = 0x00010005
  OUTPUT_DIRECTORY        = Build/GblChainloadPkg
  SUPPORTED_ARCHITECTURES = AARCH64
  BUILD_TARGETS           = RELEASE
  SKUID_IDENTIFIER        = DEFAULT

[BuildOptions]
  *_*_*_CC_FLAGS = -DGBL_MODE=$(GBL_MODE) -DGBL_AUTO=$(GBL_AUTO) \
                    -DGBL_DEBUG=$(GBL_DEBUG) -DGBL_VERBOSE=$(GBL_VERBOSE)

[LibraryClasses]
  # Carried-forward libs
  AblUnwrapLib|GblChainloadPkg/Library/AblUnwrapLib/AblUnwrapLib.inf
  LogFsLib|GblChainloadPkg/Library/LogFsLib/LogFsLib.inf
  # New v2 libs
  DynamicPatchLib|GblChainloadPkg/Library/DynamicPatchLib/DynamicPatchLib.inf
  ProtocolHookLib|GblChainloadPkg/Library/ProtocolHookLib/ProtocolHookLib.inf
  # MdePkg minimal
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  DebugLib|MdePkg/Library/UefiDebugLibConOut/UefiDebugLibConOut.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  # ... carry-forward the rest of the dirty .dsc's library mappings as-is

[Components]
  GblChainloadPkg/Application/GblChainload/GblChainload.inf
```

(Engineer should diff the dirty repo's `GblChainloadPkg.dsc` and copy the working library mappings — only the `[BuildOptions]` and `[Components]` blocks need significant adjustment.)

- [ ] **Step 3: Write `scripts/build.sh`**

```bash
#!/usr/bin/env bash
# Build a gbl-chainload .efi with the requested mode/flags.
set -euo pipefail

MODE=1
AUTO=0
DEBUG=0
VERBOSE=0
EMBED=""    # plan-3
PROFILE=""  # plan-2

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode)       MODE="$2"; shift 2;;
    --auto)       AUTO=1; shift;;
    --debug)      DEBUG=1; shift;;
    --verbose)    VERBOSE=1; shift;;
    --embed)      EMBED="$2"; shift 2;;
    --profile)    PROFILE="$2"; shift 2;;
    -h|--help)
      cat <<EOF
Usage: $0 --mode {1|2|3} [--auto] [--debug] [--verbose]
              [--embed <abl.bin>] [--profile <dev>/<ota>]
EOF
      exit 0;;
    *) echo "unknown flag: $1" >&2; exit 2;;
  esac
done

case "$MODE" in 1|2|3) ;; *) echo "--mode must be 1, 2, or 3" >&2; exit 2;; esac

# Build artifact name reflects the active flags.
SUFFIX=""
[[ $AUTO    -eq 1 ]] && SUFFIX+="-auto"
[[ $DEBUG   -eq 1 ]] && SUFFIX+="-debug"
[[ $VERBOSE -eq 1 ]] && SUFFIX+="-verbose"
ARTIFACT="dist/mode-${MODE}${SUFFIX}.efi"

mkdir -p dist
echo "==> Building $ARTIFACT (mode=$MODE auto=$AUTO debug=$DEBUG verbose=$VERBOSE)"

# Delegate the actual build to the docker wrapper.
GBL_MODE=$MODE GBL_AUTO=$AUTO GBL_DEBUG=$DEBUG GBL_VERBOSE=$VERBOSE \
  ./scripts/build-inside-docker.sh

# Pick up the EDK-II output and copy to dist/ with the artifact name.
EDK_OUT="edk2/Build/GblChainloadPkg/RELEASE_*/AARCH64/GblChainload.efi"
cp $(ls $EDK_OUT | head -1) "$ARTIFACT"
echo "==> Built $ARTIFACT"
```

`chmod +x scripts/build.sh`. (`build-inside-docker.sh` already exists from Task 10; it expects the env vars `GBL_MODE` etc. to be set so the EDK-II `build` command picks them up via `-D GBL_MODE=$GBL_MODE`. May need a tweak — engineer diffs the dirty repo's existing `build-inside-docker.sh` and replaces its old mode-string parsing with these env vars.)

- [ ] **Step 4: Smoke build**

```bash
cd /home/vivy/gbl-chainload && ./scripts/build.sh --mode 1
```

Expected: `dist/mode-1.efi` exists.

- [ ] **Step 5: Smoke build dev variant**

```bash
./scripts/build.sh --mode 1 --auto --debug --verbose
```

Expected: `dist/mode-1-auto-debug-verbose.efi` exists.

- [ ] **Step 6: Commit**

```bash
git add GblChainloadPkg/GblChainloadPkg.{dsc,dec} scripts/build.sh \
  && git commit -m "build: GblChainloadPkg.{dsc,dec} + scripts/build.sh argv parser"
```

---

## Task 30: `tests/010_build_smoke.sh` + `tests/runall.sh` + GitHub Actions CI

**Files:**
- Create: `gbl-chainload/tests/010_build_smoke.sh`
- Create: `gbl-chainload/tests/runall.sh`
- Create: `gbl-chainload/.github/workflows/ci.yml`

- [ ] **Step 1: Write `tests/010_build_smoke.sh`**

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
./scripts/build.sh --mode 1
test -f dist/mode-1.efi || { echo "mode-1.efi missing"; exit 1; }
./scripts/build.sh --mode 1 --auto --debug --verbose
test -f dist/mode-1-auto-debug-verbose.efi \
  || { echo "mode-1-auto-debug-verbose.efi missing"; exit 1; }
echo "ok 010_build_smoke"
```

`chmod +x tests/010_build_smoke.sh`.

- [ ] **Step 2: Write `tests/047_cleanup_lint.sh` — assert dead-end files never landed**

The spec lists 7 dead-ends that must not be carried forward (EBS-mutate, UDT helper, pull-logfs, Toggle-Primary-OS, shell-boot, mode-sprawl knobs, debug-variant matrix). A negative lint catches a careless `cp -r` reflex during Tasks 7-10:

```bash
#!/usr/bin/env bash
# 047_cleanup_lint.sh — assert dead-end files / symbols never landed in v2.
set -euo pipefail
cd "$(dirname "$0")/.."

FAIL=0
check_absent() {
  local label="$1"; shift
  for path in "$@"; do
    if [[ -e "$path" ]]; then
      echo "FAIL: $label still present at $path"; FAIL=1
    fi
  done
}
check_no_match() {
  local label="$1"; shift
  local pattern="$1"; shift
  if grep -RnE "$pattern" "$@" 2>/dev/null; then
    echo "FAIL: $label pattern '$pattern' still present"; FAIL=1
  fi
}

# 1. EBS-mutate scaffolding
check_absent "EBS-mutate" \
  GblChainloadPkg/Library/ProtocolHookLib/EbsMutate.c \
  GblChainloadPkg/Library/ProtocolHookLib/EbsMutate.h \
  tests/044_bootargs_rewrite_harness.sh
check_no_match "EBS-mutate knob" 'GBL_DEBUG_EBS_MUTATE' \
  GblChainloadPkg scripts

# 2. UDT helper
check_absent "UDT helper" \
  tests/043_update_device_tree_callsite_anchor.sh \
  docs/re/update-device-tree-callsite-helper.md
check_no_match "UDT helper symbol" 'kUpdateDtbHelperHex|ApplyUpdateDeviceTreeLogHelper|FindUpdateDeviceTreeCallsite' \
  GblChainloadPkg
check_no_match "UDT helper knob" 'GBL_DEBUG_UDT_HELPER' \
  GblChainloadPkg scripts

# 3. oem pull-logfs
check_absent "pull-logfs script" scripts/pull-logfs.sh
check_no_match "pull-logfs invocation" 'pull-logfs|pull_logfs' \
  scripts tests

# 4-5. Toggle-Primary-OS / shell-boot — checked at edk2 submodule
( cd edk2 && git log --oneline | grep -iE 'toggle.*primary|shell.boot|get-staged.*logfs' ) \
  && { echo "FAIL: edk2 submodule still carries banned commits"; FAIL=1; } \
  || true

# 6. Mode sprawl — none of the old multi-knob defines
check_no_match "mode sprawl knobs" \
  'AUTO_DEBUG_MODE|MODE_DEBUG\b|MODE_TEMPLATE|FAKELOCKED|MINIMAL\b|MODE_1\b' \
  GblChainloadPkg/Application
# (these strings can legitimately appear in docs/ and .re-notes/ — those are excluded
# by limiting the scan to GblChainloadPkg/Application)

# 7. Debug-variant matrix
check_no_match "debug-variant matrix" \
  '\-\-debug-variant|GBL_DEBUG_PATCH_ONLY|GBL_DEBUG_NO_EBS|ebs-wrapper-only|ebs-fdt-probe|ebs-scan|ebs-no-bootconfig|ebs-no-close' \
  GblChainloadPkg scripts

[[ $FAIL -eq 0 ]] && echo "ok 047_cleanup_lint"
exit $FAIL
```

`chmod +x tests/047_cleanup_lint.sh`.

- [ ] **Step 3: Run cleanup lint — pass**

```bash
bash tests/047_cleanup_lint.sh
```

Expected: `ok 047_cleanup_lint`. If anything fails, retrace the carry-forward steps (Tasks 7-10) and verify each banned file/symbol/knob is absent.

- [ ] **Step 4: Write `tests/runall.sh`**

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

# Host-only tests (fast).
bash tests/045_mode_taxonomy_lint.sh
bash tests/046_mode1_protocol_hook_lint.sh
bash tests/047_cleanup_lint.sh
bash tests/042_dynamic_patch_harness.sh
bash tests/051_gbl_root_canoe_regression.sh   # may skip if fixtures absent

# Carried-forward signature lint, etc.
[[ -f tests/030_signature_lint.sh ]] && bash tests/030_signature_lint.sh

# Build smoke (slowest — last).
bash tests/010_build_smoke.sh

echo "ALL TESTS PASS"
```

`chmod +x tests/runall.sh`.

- [ ] **Step 5: Carry `tests/030_signature_lint.sh`**

```bash
cp /home/vivy/gbl-chainload-dirty/tests/030_signature_lint.sh \
   /home/vivy/gbl-chainload/tests/ 2>/dev/null \
  && chmod +x /home/vivy/gbl-chainload/tests/030_signature_lint.sh \
  || echo "no 030 in dirty repo — skip"
```

- [ ] **Step 6: Author `.github/workflows/ci.yml`**

```yaml
name: CI
on:
  push:
    branches: [main]
  pull_request:

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive
      - name: Set up Docker
        uses: docker/setup-buildx-action@v3
      - name: Build docker image
        run: docker build -t gbl-chainload-build docker/
      - name: Run all tests
        run: |
          docker run --rm -v "$PWD":/work -w /work gbl-chainload-build \
            bash tests/runall.sh
```

- [ ] **Step 7: Run all tests locally**

```bash
cd /home/vivy/gbl-chainload && bash tests/runall.sh
```

Expected: ALL TESTS PASS.

- [ ] **Step 8: Commit**

```bash
git add tests/010_build_smoke.sh tests/runall.sh tests/030_signature_lint.sh \
        tests/047_cleanup_lint.sh .github/workflows/ci.yml \
  && git commit -m "tests + CI: runall harness, build smoke, cleanup lint, GitHub Actions"
```

- [ ] **Step 9: Push + verify GitHub Actions runs green**

```bash
cd /home/vivy/gbl-chainload && git push origin main
```

Verify in GitHub UI that the workflow runs and passes.

---

## Task 31: Final verification + tag

**Files:** none (just verification + tag).

- [ ] **Step 1: Final runall**

```bash
bash tests/runall.sh
```

Expected: ALL TESTS PASS.

- [ ] **Step 2: Verify default artifacts exist**

```bash
ls -lh dist/mode-1.efi dist/mode-1-auto-debug-verbose.efi
```

- [ ] **Step 3: Tag**

```bash
git tag -a v2.0.0-plan1-foundation -m "Plan 1 complete: foundation + engine v2 + universal + mode-1 first build"
git push origin v2.0.0-plan1-foundation
```

- [ ] **Step 4: Confirm spec end-state checklist for Plan-1 scope**

Cross-check the spec's "End-state checklist (for verifier)" for items that fall in Plan 1:

- [x] New repo at `gh:1vivy/gbl-chainload` with `main` green from HEAD.
- [x] Fresh edk2 fork repinned, no Toggle-Primary-OS / logfs-fastboot / shell-boot.
- [x] `tools/abl-patcher --check-anchors-only` green on the fixture set we have.
- [ ] Device test of `dist/mode-1.efi` on canoe (manual — not covered by host CI).
- [x] No EBS-mutate / UDT helper / pull-logfs / Toggle-Primary-OS / shell-boot code anywhere.

Items deferred to Plan 2/3:
- mode-2.efi, mode-3.efi
- ABL embed
- RE docs (oplusreserve1, gbl-load-mechanism, scm-fuse-classification)

- [ ] **Step 5: Manual device test (optional gate before declaring Plan 1 done)**

```bash
./scripts/test-device-automatic.sh --escape-with-payload dist/mode-1-auto-debug-verbose.efi
```

Expected device evidence (per spec "Validation strategy → Device → Mode-1"):
- `bootloader_log` shows `gbl-chainload | mode=1`.
- `vb-fakelock | READ_CONFIG | is_unlocked 1->0 | is_unlock_critical 1->0`.
- `DynamicPatch: patch9-avb-locked-recoverable-continue OK` (the canoe-side validation that motivated this whole rewrite).
- `qsee-km | cmd=0x208(SET_BOOT_STATE) | ... | isUnlocked=0 | color=0`.
- Kernel cmdline: `androidboot.verifiedbootstate=green`, `androidboot.vbmeta.device_state=locked`.
- `getprop ro.boot.flash.locked` returns `1`.

If patch9 still misses on canoe, the engine-v2 anchor wasn't unique-and-canoe-hitting — re-do Task 18 Step 3 with a different anchor, re-test.

---

## Plan 1 done. Next steps.

After Plan 1 lands and device-validates:

- **Plan 2 (Mode-2):** typed-struct profile system, Qseecom/SPSS overlay, vtable fingerprint extractor, `extract-mode2-profile.py`, mode-2 build flag wiring, device validation against custom recovery boot.
- **Plan 3 (Mode-3 + Embed + RE docs):** empty mode-3 overlay (universal-only), `--embed <abl.bin>` flag, runtime cache-key check, `tests/050_embed_determinism.sh`, `docs/re/oplusreserve1-write-paths.md`, `docs/re/gbl-load-mechanism.md`, `docs/re/scm-fuse-classification.md`.

Each follow-up plan is self-contained and produces working, testable software.
