# Release Workflow + Project Versioning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land a tag/dispatch-triggered GitHub Actions release workflow that bundles all five installer ZIPs plus Linux/Win/macOS host-tool zips (with `efisp-package.py`), driven by a single top-level `VERSION` file that also drives user-visible version surfaces (fastboot menu, getvar, on-screen banner, recovery ui_print, host-tool `--version`).

**Architecture:** Two halves, lockstep. (1) Versioning refactor: introduce `VERSION`, plumb it through `scripts/build.sh` → `build-inside-docker.sh` → EDKII `build -D` → existing `.dsc` CC_FLAGS → every `.c` already compiled by `GblChainloadPkg` (which links `FastbootLib`/`BootLib` from the `edk2/` fork). The fastboot menu and getvar additions in `edk2/QcomModulePkg/Library/{BootLib,FastbootLib}/` are linked into the same chainloaded EFI — no separate ABL build needed. (2) Release workflow: `verify → prep-image → {build-host-tools, build-zips} → release` job graph in `.github/workflows/release.yml`, gated by VERSION/tag/CHANGELOG/MANIFEST/CI-green checks, producing draft GH Releases with auto+curated notes.

**Tech Stack:** GitHub Actions, `gh` CLI, Docker (existing `gbl-chainload-build:latest` image), zig (for cross-builds, extending the existing toolchain), bash, EDK II (existing build), C, Python 3.

**Reference spec:** `docs/superpowers/specs/2026-05-20-release-workflow-design.md`.

---

## File Structure

**New files (parent repo):**
- `VERSION` — one line `X.Y.Z`. Source of truth.
- `.github/workflows/release.yml` — the workflow.
- `.github/release.yml` — auto-notes PR-label categories.
- `CHANGELOG.md` — curated notes per version.
- `tools/RELEASE_README.md` — README copied into each host-tool zip.

**Modified files (parent repo):**
- `scripts/build.sh` — export `GBL_CHAINLOAD_VERSION` from `VERSION`.
- `scripts/build-inside-docker.sh` — pass `-D GBL_CHAINLOAD_VERSION=...` to `build`.
- `scripts/build-cross-tools.sh` — add `linux` target via zig.
- `scripts/build-recovery-zip.sh` — substitute `__GBL_VERSION__` placeholder in staged `update-binary`.
- `scripts/efisp-package.py` — `--bin-dir`, `--version`, script-dir `VERSION`/`bin/` fallback.
- `GblChainloadPkg/GblChainloadPkg.dsc` — remove hardcoded `DEFINE GBL_CHAINLOAD_VERSION`.
- `GblChainloadPkg/Application/GblChainload/Entry.c` — `#error` instead of `"v2"` fallback; SCR_PRINT lines gain version prefix.
- `tools/<each>/Makefile` (7 tools) — `-DGBL_TOOL_VERSION='"X.Y.Z"'` from VERSION at make time.
- `tools/<each>/<tool>.c` (7 tools) — `--version` flag handling + usage banner update.
- `tools/gbl-pack/gbl-pack.c` — replace hardcoded `"gbl-pack 1.0.0"`.
- `edk2/QcomModulePkg/Library/BootLib/FastbootMenu.c` — add `VERSION - X.Y.Z` row.
- `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c` — add `gbl-chainload_version` getvar.

**Modified files (zip submodule):**
- `zip/META-INF/com/google/android/update-binary` — `ui_print "gbl-chainload v__GBL_VERSION__"` header line.

---

## Task 1: VERSION file + build pipeline + Entry.c remnant

**Goal:** Plumb a top-level `VERSION` file through the EDK II build so every `.c` compiled by `GblChainloadPkg.dsc` (including the FastbootLib/BootLib sources linked from `edk2/`) sees the macro `GBL_CHAINLOAD_VERSION` set to the file's contents. Remove the existing `"2.0"` DSC default and the `"v2"` Entry.c fallback so any future build with a missing macro is a hard error.

**Files:**
- Create: `VERSION`
- Modify: `scripts/build.sh`
- Modify: `scripts/build-inside-docker.sh`
- Modify: `GblChainloadPkg/GblChainloadPkg.dsc`
- Modify: `GblChainloadPkg/Application/GblChainload/Entry.c`

**Acceptance Criteria:**
- [ ] `VERSION` exists at repo root, contains `2.2.0`, single line, no trailing newline confusion (one `\n` at EOF).
- [ ] `scripts/build.sh --mode 0` succeeds end-to-end on a clean tree.
- [ ] Resulting `dist/mode-0.efi` contains the literal string `2.2.0` (verified via `strings dist/mode-0.efi | grep -F 2.2.0`).
- [ ] `GblChainloadPkg.dsc` no longer contains `DEFINE GBL_CHAINLOAD_VERSION` (verified via `! grep -n 'DEFINE GBL_CHAINLOAD_VERSION' GblChainloadPkg/GblChainloadPkg.dsc`).
- [ ] `Entry.c` line range 35–45 contains an `#error` and no `"v2"` fallback string.

**Verify:** `echo 2.2.0 > VERSION && bash scripts/build.sh --mode 0 && strings dist/mode-0.efi | grep -cF 2.2.0` returns ≥1.

**Steps:**

- [ ] **Step 1: Create VERSION file**

```bash
printf '2.2.0\n' > VERSION
```

- [ ] **Step 2: Modify `scripts/build.sh` — read VERSION, export GBL_CHAINLOAD_VERSION**

Insert after the existing `cd "$REPO_ROOT"` (around line 10):

```bash
# Single source of truth for the in-binary version string. Build aborts
# with a missing-file error rather than guessing — the .dsc default and
# Entry.c fallback were removed when VERSION was introduced.
if [[ ! -f VERSION ]]; then
  echo "error: VERSION file missing at $REPO_ROOT/VERSION" >&2
  exit 1
fi
GBL_CHAINLOAD_VERSION="$(tr -d '[:space:]' < VERSION)"
export GBL_CHAINLOAD_VERSION
```

Add `-e GBL_CHAINLOAD_VERSION="$GBL_CHAINLOAD_VERSION" \` to the `docker run` argument list (alongside `-e GBL_MODE`, etc., around line 81).

- [ ] **Step 3: Modify `scripts/build-inside-docker.sh` — propagate macro to EDKII build**

Around line 16 (the existing env defaults), add:

```bash
GBL_CHAINLOAD_VERSION="${GBL_CHAINLOAD_VERSION:?GBL_CHAINLOAD_VERSION must be exported by build.sh}"
```

Append a `-D GBL_CHAINLOAD_VERSION="$GBL_CHAINLOAD_VERSION"` line to the `build \` invocation (after the existing `-D GBL_BUILD_NAME=...` line, around line 76).

- [ ] **Step 4: Modify `GblChainloadPkg/GblChainloadPkg.dsc` — drop hardcoded DEFINE**

Delete line 60:
```
  DEFINE GBL_CHAINLOAD_VERSION            = 2.0
```

The CC_FLAGS line (currently around line 154) referencing `$(GBL_CHAINLOAD_VERSION)` stays. It now picks up the value from the `-D` passed by `build-inside-docker.sh`.

- [ ] **Step 5: Modify `Entry.c` — replace `"v2"` fallback with #error**

Replace lines 38–41:

```c
/* GBL_CHAINLOAD_VERSION is injected by the build (scripts/build.sh reads VERSION). */
#ifndef GBL_CHAINLOAD_VERSION
#error "GBL_CHAINLOAD_VERSION must be defined by the build (VERSION file)"
#endif
```

- [ ] **Step 6: Build + verify**

```bash
bash scripts/build.sh --mode 0
strings dist/mode-0.efi | grep -cF 2.2.0
```

Expected: build succeeds; grep returns ≥1.

- [ ] **Step 7: Commit**

```bash
git add VERSION scripts/build.sh scripts/build-inside-docker.sh \
        GblChainloadPkg/GblChainloadPkg.dsc \
        GblChainloadPkg/Application/GblChainload/Entry.c
git commit -m "feat(version): introduce VERSION file as single source of truth

Plumb VERSION through scripts/build.sh → build-inside-docker.sh →
EDKII build -D → existing .dsc CC_FLAGS. Drop the .dsc 2.0 default
and the Entry.c 'v2' fallback so missing-macro builds hard-fail."
```

---

## Task 2: Fastboot menu row + getvar (edk2 fork)

**Goal:** Surface the version on two user-visible fastboot surfaces that already exist in the linked FastbootLib/BootLib — a `VERSION - X.Y.Z` row in the menu UI and a `gbl-chainload_version` getvar. These are edits in `edk2/` (allowed per the project's forked-edk2 policy).

**Files:**
- Modify: `edk2/QcomModulePkg/Library/BootLib/FastbootMenu.c`
- Modify: `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c`

**Acceptance Criteria:**
- [ ] `FastbootMenu.c` declares a new menu row `{{"VERSION - " GBL_CHAINLOAD_VERSION}, …}` immediately after the existing `MODE - <mode>` row.
- [ ] `FastbootCmds.c` calls `FastbootPublishVar ("gbl-chainload_version", GBL_CHAINLOAD_VERSION)` alongside the existing `gbl-chainload_mode`/`_date`/`_auto` publishes.
- [ ] Rebuilt `dist/mode-0.efi` contains both new literals (`VERSION - 2.2.0` and `gbl-chainload_version`).
- [ ] No existing menu row or getvar is broken (visual diff of the menu row array shows additive change).

**Verify:** `bash scripts/build.sh --mode 0 && strings dist/mode-0.efi | grep -E '^(VERSION - 2\.2\.0|gbl-chainload_version)$' | sort -u | wc -l` returns `2`.

**Steps:**

- [ ] **Step 1: Branch the edk2 submodule**

```bash
cd edk2
git checkout -b feat/version-surfaces main
cd ..
```

- [ ] **Step 2: Modify `FastbootMenu.c`**

Read lines 270–300 to find the menu row array, then insert immediately after the `MODE - " GBL_CHAINLOAD_MODE` row (around line 281):

```c
    {{"VERSION - " GBL_CHAINLOAD_VERSION},
        BIG_FACTOR, NO_INVERT, NO_OPTION,        common_msg_textColor},
```

Match the surrounding row's struct field initializers exactly.

- [ ] **Step 3: Modify `FastbootCmds.c`**

After the `FastbootPublishVar ("gbl-chainload_date", ...)` line (around line 5076), insert:

```c
  FastbootPublishVar ("gbl-chainload_version", GBL_CHAINLOAD_VERSION);
```

- [ ] **Step 4: Build + verify both literals reach the EFI**

```bash
bash scripts/build.sh --mode 0
strings dist/mode-0.efi | grep -E '^(VERSION - 2\.2\.0|gbl-chainload_version)$' | sort -u
```

Expected: two distinct lines.

- [ ] **Step 5: Commit in edk2 submodule, then bump parent**

```bash
cd edk2
git add QcomModulePkg/Library/BootLib/FastbootMenu.c \
        QcomModulePkg/Library/FastbootLib/FastbootCmds.c
git commit -m "feat: surface gbl-chainload version on menu + getvar

Adds a VERSION - X.Y.Z row to FastbootMenu and a
gbl-chainload_version fastboot getvar. Value comes from
GBL_CHAINLOAD_VERSION macro injected by the parent's build."
git push origin feat/version-surfaces
# Open PR or merge to main per fork's own workflow.
cd ..
git add edk2
git commit -m "submodule: bump edk2 for version-surface additions"
```

(If the edk2 fork uses direct-to-main pushes the same way the parent does, the branch+PR step collapses.)

---

## Task 3: Host tools — `--version` flag, Makefile wiring, gbl-pack remnant

> **USER-ORDERED GATE — NON-SKIPPABLE.** This task was requested by the user in the current conversation ("Just make sure old versioning remnants are removed / properly addressed then. the project has grown so it'll need finding."). It MUST NOT be closed by walking around it, by declaring it "verified inline", or by substituting a cheaper check. Close only after every item in `acceptanceCriteria` has been re-validated independently, with output captured.

**Goal:** Each of the seven host tools (`fv-unwrap`, `abl-patcher`, `gbl-pack`, `gbl-commit`, `vbmeta-graft`, `mode2-profile`, `gblp1-inspect`) accepts `--version`, prints VERSION on stdout, and exits 0. The hardcoded `"gbl-pack 1.0.0"` remnant in `gbl-pack.c` is replaced with the macro. No project-version remnant survives outside the protocol/schema/Android-domain list documented in spec §4.4.

**Files:**
- Modify: `tools/fv-unwrap/Makefile`, `tools/fv-unwrap/fv-unwrap.c`
- Modify: `tools/abl-patcher/Makefile`, `tools/abl-patcher/abl-patcher.c`
- Modify: `tools/gbl-pack/Makefile`, `tools/gbl-pack/gbl-pack.c`
- Modify: `tools/gbl-commit/Makefile`, `tools/gbl-commit/gbl-commit.c`
- Modify: `tools/vbmeta-graft/Makefile`, `tools/vbmeta-graft/vbmeta-graft.c`
- Modify: `tools/mode2-profile/Makefile`, `tools/mode2-profile/mode2-profile.c`
- Modify: `tools/gblp1-inspect/Makefile`, `tools/gblp1-inspect/gblp1-inspect.c`

**Acceptance Criteria:**
- [ ] Every tool's Makefile contains a `GBL_TOOL_VERSION := $(shell tr -d '[:space:]' < $(SELF)/../../VERSION)` (or equivalent path) and adds `-DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'` to CFLAGS.
- [ ] Every tool's `<tool>.c` handles `argv[1] == "--version"` by printing `<tool> X.Y.Z\n` and exiting 0.
- [ ] `tools/gbl-pack/gbl-pack.c:64` reads `in.packer_version = "gbl-pack " GBL_TOOL_VERSION;` — no literal `"1.0.0"`.
- [ ] `grep -rn -E 'gbl[-_]chainload.*v?[0-9]+\.[0-9]+\.[0-9]+|"v?2\.0[^.0-9]' --include='*.c' --include='*.h' --include='*.sh' --include='*.py' --exclude-dir=edk2 --exclude-dir=Build --exclude-dir=dist .` returns ZERO matches outside `VERSION`, `CHANGELOG.md`, `docs/`, and `tools/RELEASE_README.md`.
- [ ] After cross-building, each of the seven binaries prints VERSION when invoked as `<tool> --version` (verified for the local-arch build; cross-builds covered by Task 4).

**Verify:**

```bash
bash scripts/build-recovery-tools.sh
for t in fv-unwrap abl-patcher gbl-pack gbl-commit vbmeta-graft mode2-profile gblp1-inspect; do
  out=$(./dist/recovery/$t --version 2>&1 || true)
  echo "$t -> $out"
  printf '%s' "$out" | grep -qF "$(cat VERSION)" || { echo "FAIL: $t did not print VERSION"; exit 1; }
done
# Remnant scan:
! grep -rn -E '"gbl-pack 1\.0\.0"|"v2"' --include='*.c' --include='*.h' .
```

Expected: every tool prints the VERSION; final grep returns no hits.

**Steps:**

- [ ] **Step 1: Audit current Makefile pattern**

Run:
```bash
head -20 tools/fv-unwrap/Makefile
```

Note the existing `CFLAGS`/`android`/`windows`/`macos`/`linux` target pattern so the VERSION wiring lands in a place all targets see it.

- [ ] **Step 2: Update one tool's Makefile as the reference**

Pick `tools/fv-unwrap/Makefile`. Near the top (after the existing CC/AR/etc. variables, before the first `CFLAGS` line), insert:

```make
# Project version (single source of truth: top-level VERSION file).
GBL_TOOL_VERSION := $(shell tr -d '[:space:]' < $(dir $(lastword $(MAKEFILE_LIST)))../../VERSION)
ifeq ($(GBL_TOOL_VERSION),)
$(error VERSION file is empty or missing)
endif
CFLAGS += -DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'
```

Repeat the same three-line block (identical body) in each of the other six tool Makefiles. The `dir` + `lastword $(MAKEFILE_LIST)` pattern keeps the path resolution local to each Makefile and unaffected by where `make` is invoked from.

- [ ] **Step 3: Add `--version` handling — establish the pattern**

In `tools/fv-unwrap/fv-unwrap.c`, locate the existing argv parsing in `main()`. Before any other argv handling, add:

```c
if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
    printf("fv-unwrap %s\n", GBL_TOOL_VERSION);
    return 0;
}
```

Also update the usage banner string from e.g. `"Usage: fv-unwrap ..."` to `"fv-unwrap " GBL_TOOL_VERSION "\nUsage: fv-unwrap ..."`.

Repeat the same pattern in `abl-patcher.c`, `gbl-pack.c`, `gbl-commit.c`, `vbmeta-graft.c`, `mode2-profile.c`, `gblp1-inspect.c`. The literal tool name in each `printf` varies — match the actual binary name.

- [ ] **Step 4: Remove `gbl-pack` 1.0.0 remnant**

In `tools/gbl-pack/gbl-pack.c` around line 64:

```c
in.packer_version = "gbl-pack " GBL_TOOL_VERSION;
```

(Adjacent string-literal concatenation — C standard, no runtime cost.)

- [ ] **Step 5: Build + verify all seven tools print VERSION**

```bash
bash scripts/build-recovery-tools.sh
for t in fv-unwrap abl-patcher gbl-pack gbl-commit vbmeta-graft mode2-profile gblp1-inspect; do
  out=$(./dist/recovery/$t --version 2>&1 || true)
  echo "$t -> $out"
  printf '%s' "$out" | grep -qF "$(cat VERSION)" || { echo "FAIL: $t"; exit 1; }
done
```

Expected: seven lines, each `<tool> 2.2.0`.

- [ ] **Step 6: Remnant scan**

```bash
grep -rn -E '"gbl-pack 1\.0\.0"|"v2"' --include='*.c' --include='*.h' --exclude-dir=edk2 --exclude-dir=Build .
grep -rn -E '"\b[0-9]\.[0-9]\b"' tools/ --include='*.c' --include='*.h' | grep -v -iE '(packer_version|version =|/\*|//)' || true
```

Expected: first command returns no lines; second is a sanity check (any hits should be hand-reviewed and confirmed protocol-version literals before closing the task).

- [ ] **Step 7: Commit**

```bash
git add tools/
git commit -m "feat(tools): --version flag + GBL_TOOL_VERSION macro from VERSION

All seven host tools accept --version and print the project version.
Removes the hardcoded 'gbl-pack 1.0.0' string in gbl-pack.c."
```

```json:metadata
{"files": ["tools/fv-unwrap/Makefile", "tools/fv-unwrap/fv-unwrap.c", "tools/abl-patcher/Makefile", "tools/abl-patcher/abl-patcher.c", "tools/gbl-pack/Makefile", "tools/gbl-pack/gbl-pack.c", "tools/gbl-commit/Makefile", "tools/gbl-commit/gbl-commit.c", "tools/vbmeta-graft/Makefile", "tools/vbmeta-graft/vbmeta-graft.c", "tools/mode2-profile/Makefile", "tools/mode2-profile/mode2-profile.c", "tools/gblp1-inspect/Makefile", "tools/gblp1-inspect/gblp1-inspect.c"], "verifyCommand": "bash scripts/build-recovery-tools.sh && for t in fv-unwrap abl-patcher gbl-pack gbl-commit vbmeta-graft mode2-profile gblp1-inspect; do out=$(./dist/recovery/$t --version 2>&1 || true); printf '%s' \"$out\" | grep -qF \"$(cat VERSION)\" || exit 1; done && ! grep -rn -E '\"gbl-pack 1\\.0\\.0\"|\"v2\"' --include='*.c' --include='*.h' --exclude-dir=edk2 --exclude-dir=Build .", "acceptanceCriteria": ["every tool Makefile sources GBL_TOOL_VERSION from VERSION", "every tool's main() handles --version printing VERSION and exiting 0", "gbl-pack.c packer_version uses macro, no literal 1.0.0", "remnant grep returns zero non-edk2/non-Build matches", "all seven binaries print VERSION at --version"], "userGate": true, "tags": ["user-gate"]}
```

---

## Task 4: `scripts/build-cross-tools.sh` — add Linux x86_64 target

**Goal:** Extend the existing windows+macos zig cross-build script with a `linux` target producing static x86_64 binaries; `all` becomes win+macos+linux.

**Files:**
- Modify: `scripts/build-cross-tools.sh`
- Modify: `tools/<each>/Makefile` (add a `linux` target alongside existing `windows`/`macos`/`android`)

**Acceptance Criteria:**
- [ ] `scripts/build-cross-tools.sh linux` exits 0 and produces `dist/linux/<tool>` for every tool plus `dist/linux/SHA256SUMS`.
- [ ] Each `dist/linux/<tool>` is a 64-bit ELF (`file dist/linux/fv-unwrap` mentions `ELF 64-bit … x86_64 … statically linked`).
- [ ] `scripts/build-cross-tools.sh all` succeeds and populates `dist/{windows,macos,linux}` (all three).
- [ ] Running `dist/linux/fv-unwrap --version` on the ubuntu-latest runner prints VERSION (smoke-test in CI).

**Verify:**

```bash
bash scripts/build-cross-tools.sh linux
ls dist/linux/
file dist/linux/fv-unwrap | grep -q 'ELF 64-bit.*x86_64.*statically linked'
./dist/linux/fv-unwrap --version | grep -qF "$(cat VERSION)"
```

Expected: directory contains seven binaries + `SHA256SUMS`; `file` check passes; `--version` prints `2.2.0`.

**Steps:**

- [ ] **Step 1: Read current script to confirm structure**

```bash
sed -n '1,60p' scripts/build-cross-tools.sh
```

- [ ] **Step 2: Add `linux` case + extend `all`**

Update the case validator near line 14:

```bash
case "$OS" in
  windows|macos|linux|all) ;;
  *) echo "usage: $0 windows|macos|linux|all" >&2; exit 2 ;;
esac
```

Add a linux block inside the `docker run` heredoc (alongside the existing windows/macos blocks):

```bash
  if [ "$OS" = linux ] || [ "$OS" = all ]; then
    mkdir -p dist/linux
    for t in $TOOLS; do
      make -C tools/$t clean
      make -C tools/$t linux
      install -Dm755 tools/$t/$t-linux dist/linux/$t
    done
    ( cd dist/linux && sha256sum * > SHA256SUMS )
  fi
```

Also extend the `TOOLS` variable on line 23 to include `gblp1-inspect`:

```bash
TOOLS="fv-unwrap abl-patcher gbl-pack gbl-commit vbmeta-graft mode2-profile gblp1-inspect"
```

- [ ] **Step 3: Add `linux` target in each Makefile**

Identify the existing `windows`/`macos` target pattern (likely uses `zig cc -target x86_64-windows-gnu` / `aarch64-macos`/`x86_64-macos` lipo). Add for each Makefile:

```make
linux: $(SRC)
	zig cc -target x86_64-linux-musl -static \
	    $(CFLAGS) $(SRC) -o $(NAME)-linux
```

(Exact `$(SRC)`/`$(NAME)` variable names vary per Makefile — match the existing windows/macos targets.)

- [ ] **Step 4: Build + verify**

```bash
bash scripts/build-cross-tools.sh linux
ls dist/linux/
file dist/linux/fv-unwrap
./dist/linux/fv-unwrap --version
```

Expected: 8 files in `dist/linux/` (7 binaries + SHA256SUMS); `file` reports ELF x86_64 static; `--version` prints `2.2.0`.

- [ ] **Step 5: Verify `all` still works**

```bash
rm -rf dist/{windows,macos,linux}
bash scripts/build-cross-tools.sh all
ls dist/windows | wc -l   # should be 8
ls dist/macos   | wc -l   # should be 8
ls dist/linux   | wc -l   # should be 8
```

- [ ] **Step 6: Commit**

```bash
git add scripts/build-cross-tools.sh tools/
git commit -m "feat(cross): add Linux x86_64 (zig musl-static) target

Extends build-cross-tools.sh and per-tool Makefiles with a linux
target alongside the existing windows/macos zig builds. Includes
gblp1-inspect in the tool list."
```

---

## Task 5: `scripts/efisp-package.py` — `--bin-dir`, `--version`, script-dir fallback

**Goal:** The python helper becomes self-contained inside an unzipped tool bundle. Without args it finds binaries via `${script_dir}/bin/`; `--version` prints VERSION read from `${script_dir}/VERSION`.

**Files:**
- Modify: `scripts/efisp-package.py`

**Acceptance Criteria:**
- [ ] `python3 scripts/efisp-package.py --version` prints the contents of `VERSION` and exits 0.
- [ ] When invoked as `efisp-package.py --bin-dir /some/path …`, the resolved tool paths use `/some/path/<tool>`.
- [ ] When `--bin-dir` is omitted and a `bin/` directory exists adjacent to the script, that directory wins over `$PATH`.
- [ ] When `--bin-dir` is omitted AND no adjacent `bin/`, behavior unchanged from today (PATH lookup).

**Verify:**

```bash
python3 scripts/efisp-package.py --version
# Expected: 2.2.0

mkdir -p /tmp/efp-test/bin
cp dist/linux/fv-unwrap /tmp/efp-test/bin/
cp scripts/efisp-package.py /tmp/efp-test/
cp VERSION /tmp/efp-test/
python3 /tmp/efp-test/efisp-package.py --version
# Expected: 2.2.0  (read from /tmp/efp-test/VERSION)
```

**Steps:**

- [ ] **Step 1: Add `--version` flag and `--bin-dir` flag to argparse**

Locate the existing `argparse.ArgumentParser(...)` setup. Add:

```python
parser.add_argument("--version", action="store_true",
                    help="print the gbl-chainload version and exit")
parser.add_argument("--bin-dir",
                    help="directory containing the host-tool binaries "
                         "(default: ./bin next to this script, then $PATH)")
```

- [ ] **Step 2: Implement `--version` handling immediately after `parse_args()`**

```python
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

def _read_version() -> str:
    for candidate in (SCRIPT_DIR, os.path.dirname(SCRIPT_DIR)):
        p = os.path.join(candidate, "VERSION")
        if os.path.isfile(p):
            with open(p) as f:
                return f.read().strip()
    return "unknown"

args = parser.parse_args()
if args.version:
    print(_read_version())
    sys.exit(0)
```

- [ ] **Step 3: Implement `--bin-dir` + script-dir fallback**

Replace the existing `shutil.which(tool)` logic with:

```python
def _resolve_tool(name: str, override: str | None) -> str:
    if override:
        p = os.path.join(override, name)
        if not os.path.isfile(p):
            die(f"--bin-dir does not contain '{name}': {p}")
        return p
    # Try script-relative bin/ first.
    p = os.path.join(SCRIPT_DIR, "bin", name)
    if os.path.isfile(p):
        return p
    # Fall back to $PATH.
    p = shutil.which(name)
    if not p:
        die(f"tool '{name}' not found in --bin-dir or {SCRIPT_DIR}/bin "
            "or $PATH")
    return p
```

Update all call sites that previously did `shutil.which(t)` to call `_resolve_tool(t, args.bin_dir)` instead.

- [ ] **Step 4: Verify**

```bash
python3 scripts/efisp-package.py --version
# Expected: 2.2.0

mkdir -p /tmp/efp-test/bin
cp dist/linux/fv-unwrap /tmp/efp-test/bin/
cp scripts/efisp-package.py /tmp/efp-test/
cp VERSION /tmp/efp-test/
python3 /tmp/efp-test/efisp-package.py --version
# Expected: 2.2.0
```

- [ ] **Step 5: Commit**

```bash
git add scripts/efisp-package.py
git commit -m "feat(efisp-package): --bin-dir, --version, script-dir fallback

Makes efisp-package.py self-contained inside an unzipped tool
bundle: VERSION read from script dir, bin/ resolved adjacent to
the script when --bin-dir not given."
```

---

## Task 6: `build-recovery-zip.sh` + `update-binary` — VERSION substitution

**Goal:** Installer ZIPs greet the user with `gbl-chainload v<VERSION>` on the recovery screen. `build-recovery-zip.sh` substitutes a `__GBL_VERSION__` placeholder in the staged `update-binary` before packaging; the zip submodule's `update-binary` ships with that placeholder + a `ui_print` line.

**Files:**
- Modify: `scripts/build-recovery-zip.sh`
- Modify: `zip/META-INF/com/google/android/update-binary` (in zip submodule)

**Acceptance Criteria:**
- [ ] `zip/META-INF/com/google/android/update-binary` contains the literal token `__GBL_VERSION__` exactly once, in a `ui_print` line near the top of the script.
- [ ] `scripts/build-recovery-zip.sh --mode diag` produces a ZIP whose internal `update-binary` contains `gbl-chainload v2.2.0` and no `__GBL_VERSION__` placeholder.
- [ ] All five modes still build successfully end-to-end.

**Verify:**

```bash
for m in diag graft mode-0-install mode-1-install mode-2-install; do
  bash scripts/build-recovery-zip.sh --mode "$m"
  unzip -p dist/gbl-chainload-${m}.zip META-INF/com/google/android/update-binary \
    | grep -F "gbl-chainload v$(cat VERSION)" \
    || { echo "FAIL: $m"; exit 1; }
  unzip -p dist/gbl-chainload-${m}.zip META-INF/com/google/android/update-binary \
    | grep -q '__GBL_VERSION__' \
    && { echo "FAIL: placeholder leaked in $m"; exit 1; }
done
echo "OK: all five ZIPs carry version + no placeholder"
```

**Steps:**

- [ ] **Step 1: Modify zip submodule's `update-binary`**

In the zip submodule (`/home/vivy/gbl-chainload/zip`), read the current top of `META-INF/com/google/android/update-binary` to find a suitable spot for the ui_print line (typically right after the shebang + early echo). Insert:

```sh
ui_print "----------------------------------------"
ui_print "gbl-chainload v__GBL_VERSION__"
ui_print "----------------------------------------"
```

- [ ] **Step 2: Modify `scripts/build-recovery-zip.sh` to substitute placeholder**

The script currently `cp -r zip/...` into a staging area then `zip -r` it. After the staging copy but before the `zip` call, add:

```bash
# Substitute the VERSION placeholder in update-binary.
GBL_VERSION="$(tr -d '[:space:]' < "$ROOT/VERSION")"
sed -i "s|__GBL_VERSION__|${GBL_VERSION}|g" \
    "$STAGE/META-INF/com/google/android/update-binary"
# Belt-and-braces: refuse to ship a ZIP that still contains the placeholder.
if grep -q '__GBL_VERSION__' "$STAGE/META-INF/com/google/android/update-binary"; then
  echo "error: placeholder __GBL_VERSION__ still in update-binary after substitution" >&2
  exit 1
fi
```

(Variable names — `$STAGE`, `$ROOT` — must match what the existing script uses. Read the script to confirm the staging variable name before writing.)

- [ ] **Step 3: Build + verify all five modes**

```bash
for m in diag graft mode-0-install mode-1-install mode-2-install; do
  bash scripts/build-recovery-zip.sh --mode "$m"
  unzip -p dist/gbl-chainload-${m}.zip META-INF/com/google/android/update-binary \
    | grep -F "gbl-chainload v$(cat VERSION)"
done
```

Expected: five matching lines (one per mode).

- [ ] **Step 4: Commit (both repos)**

```bash
cd zip
git add META-INF/com/google/android/update-binary
git commit -m "feat: ui_print gbl-chainload version header (substituted at build)"
git push origin main
cd ..
git add zip scripts/build-recovery-zip.sh
git commit -m "feat(zip): substitute __GBL_VERSION__ placeholder in update-binary

build-recovery-zip.sh now reads top-level VERSION and substitutes
the placeholder in the staged update-binary before packaging.
Hard-fails if substitution didn't take."
```

(Per project rules: zip submodule push direct to main is allowed only with user permission; otherwise raise a zip PR. Coordinator should ask if unsure.)

---

## Task 7: Static release-time files — RELEASE_README, .github/release.yml, CHANGELOG

**Goal:** Land the three hand-curated text files the release workflow consumes (PR-list grouping, prologue source, per-platform README).

**Files:**
- Create: `tools/RELEASE_README.md`
- Create: `.github/release.yml`
- Create: `CHANGELOG.md`

**Acceptance Criteria:**
- [ ] `tools/RELEASE_README.md` exists, ≥30 lines, covers each tool's purpose in one sentence + a "how to use" pointer + the SHA256SUMS verification command + a link to the project repo.
- [ ] `.github/release.yml` parses as valid YAML (`python3 -c 'import yaml; yaml.safe_load(open(".github/release.yml"))'` exits 0) and contains the four categories from the spec (Features, Fixes, Internals, Other).
- [ ] `CHANGELOG.md` exists with at least one `## v2.2.0 — YYYY-MM-DD` heading and a non-empty body underneath.

**Verify:**

```bash
test -f tools/RELEASE_README.md && wc -l tools/RELEASE_README.md
python3 -c 'import yaml; yaml.safe_load(open(".github/release.yml"))' && echo "YAML OK"
grep -E '^## v2\.2\.0' CHANGELOG.md
```

**Steps:**

- [ ] **Step 1: Write `tools/RELEASE_README.md`**

```markdown
# gbl-chainload host tools

This bundle ships with a `VERSION` file (also printed by each tool via
`<tool> --version` or `efisp-package.py --version`).

## What's inside

| Tool | Purpose |
|---|---|
| `fv-unwrap` | Extract PE32 payload from EDK-II FV container (`abl.img` → `.efi`) |
| `abl-patcher` | Apply mode-N patches to a stock ABL binary |
| `gbl-pack` | Build a GBLP1 overlay payload |
| `gbl-commit` | Inspect / validate a packed payload |
| `vbmeta-graft` | Graft a stock OEM vbmeta onto a custom image; `list-hash` subcmd |
| `mode2-profile` | Compile mode-2 user profile (TOML → binary container) |
| `gblp1-inspect` | Parse + verify a GBLP1 container; per-entry SHA256 check |

## Verifying the download

```bash
sha256sum -c SHA256SUMS
```

All seven binaries should report `OK`. If any line reports `FAILED`, do not use the bundle — re-download.

## Using the tools

`efisp-package.py` chains the tools to produce a flash-ready EFISP payload off-device. From the unzipped bundle:

```bash
python3 efisp-package.py --help
python3 efisp-package.py --version
```

The script auto-discovers binaries in the adjacent `bin/` directory. Override with `--bin-dir <path>` if needed.

Individual tools accept `--version` and print a usage banner without arguments.

## Project

Repository: https://github.com/1vivy/gbl-chainload
```

- [ ] **Step 2: Write `.github/release.yml`**

```yaml
changelog:
  categories:
    - title: Features
      labels: [feature, enhancement]
    - title: Fixes
      labels: [bug, fix]
    - title: Internals
      labels: [refactor, internal, ci, test, docs]
    - title: Other
      labels: ["*"]
```

- [ ] **Step 3: Write `CHANGELOG.md` (seed with v2.2.0 + reconstructed prior tags)**

```markdown
# Changelog

## v2.2.0 — 2026-05-XX

Highlights:

- Release workflow + single-source `VERSION` file driving all consumers (`.dsc`, host tool Makefiles, installer `ui_print`, fastboot menu row, `gbl-chainload_version` getvar, `efisp-package.py --version`).
- Linux x86_64 host-tool builds added alongside Windows/macOS.
- Diagnostic mode shipped (pre-reboot EFISP install confidence + `/sdcard/` bundle, `gblp1-inspect`, `vbmeta-graft list-hash`).
- Universal TZ rollback-bump drop.
- AVB parser consolidation onto AvbParseLib.

Upgrade notes:

- The hardcoded `GBL_CHAINLOAD_VERSION = 2.0` in `.dsc` is gone — builds now require the `VERSION` file at repo root.
- Host tools accept `--version` and the legacy `gbl-pack 1.0.0` self-identification is gone.

## v2.1.0

(reconstructed from history — Mode-2 ZIP implementation, TOML profile migration, edk2 escape fix.)

## v2.0.0

(reconstructed from history — initial 2.x foundation.)
```

(The `2026-05-XX` placeholder gets updated to the actual release date before tagging.)

- [ ] **Step 4: Verify**

```bash
test -f tools/RELEASE_README.md
python3 -c 'import yaml; yaml.safe_load(open(".github/release.yml"))'
grep -E '^## v2\.2\.0' CHANGELOG.md
```

- [ ] **Step 5: Commit**

```bash
git add tools/RELEASE_README.md .github/release.yml CHANGELOG.md
git commit -m "docs: seed CHANGELOG, .github/release.yml, tools README

Static inputs for the release workflow: PR-label grouping for
auto-generated notes, hand-curated changelog prologue source, and
the README each host-tool zip ships with."
```

---

## Task 8: `.github/workflows/release.yml` — the workflow

**Goal:** The complete release workflow per spec §3 (4 jobs: verify → prep-image → {build-host-tools, build-zips} → release). Triggers on `v*` tag push and `workflow_dispatch`. Posts a draft GitHub Release with all eight artifacts + SHA256SUMS + curated+auto notes.

**Files:**
- Create: `.github/workflows/release.yml`

**Acceptance Criteria:**
- [ ] File parses cleanly as YAML.
- [ ] `actionlint` (if installed) reports no errors.
- [ ] Workflow contains exactly the four jobs `verify`, `prep-image`, `build-host-tools`, `build-zips`, `release`, with the correct `needs:` graph.
- [ ] `verify` performs: regex check of version, VERSION-vs-tag equality, CHANGELOG `^## v<ver>` presence, CI-green-at-this-SHA via `gh api`, MANIFEST drift check.
- [ ] `prep-image` uses `docker/build-push-action@v5` with `cache-to: type=gha,mode=max` and `cache-from: type=gha`.
- [ ] `build-host-tools` invokes `scripts/build-cross-tools.sh all`, then assembles three per-platform zips with the layout from spec §3.3.
- [ ] `build-zips` invokes `scripts/build-recovery-zip.sh --mode <m>` for all five modes and renames artifacts to carry `v<ver>`.
- [ ] `release` computes a top-level `SHA256SUMS`, builds release-notes.md (CHANGELOG prologue + `---` + auto PR list from `gh api /releases/generate-notes`), and runs a single `gh release create --draft --notes-file release-notes.md` with all nine assets.
- [ ] On `workflow_dispatch`, the draft is attached to `--target <sha>`; `gh release create` creates the `v<ver>` tag if absent (release without a tag isn't coherent).

**Verify:**

```bash
python3 -c 'import yaml; wf = yaml.safe_load(open(".github/workflows/release.yml")); jobs = wf["jobs"]; assert set(jobs) == {"verify", "prep-image", "build-host-tools", "build-zips", "release"}, jobs; print("jobs OK"); print("graph:"); [print(f"  {n}: needs={j.get(\"needs\")}") for n, j in jobs.items()]'
command -v actionlint >/dev/null 2>&1 && actionlint .github/workflows/release.yml || echo "(actionlint not installed; skipped)"
```

**Steps:**

- [ ] **Step 1: Write the workflow file**

Path: `.github/workflows/release.yml`. The skeleton (key fields shown — full body fills in the obvious bits):

```yaml
name: release

on:
  push:
    tags: ['v*']
  workflow_dispatch:
    inputs:
      version:
        description: 'Release version (X.Y.Z, no leading v)'
        required: true
        type: string

permissions:
  contents: write  # for gh release create
  packages: write  # for ghcr docker cache (if used)

jobs:
  verify:
    runs-on: ubuntu-latest
    outputs:
      version: ${{ steps.resolve.outputs.version }}
      sha: ${{ steps.resolve.outputs.sha }}
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive
          fetch-depth: 0
      - name: Resolve version
        id: resolve
        run: |
          set -euo pipefail
          if [[ "${{ github.event_name }}" == "push" ]]; then
            ver="${GITHUB_REF_NAME#v}"
          else
            ver="${{ inputs.version }}"
          fi
          [[ "$ver" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "::error::version '$ver' is not X.Y.Z"; exit 1; }
          file_ver=$(tr -d '[:space:]' < VERSION)
          [[ "$ver" == "$file_ver" ]] || { echo "::error::version '$ver' != VERSION '$file_ver'"; exit 1; }
          echo "version=$ver" >> "$GITHUB_OUTPUT"
          echo "sha=$GITHUB_SHA" >> "$GITHUB_OUTPUT"
      - name: Check CHANGELOG section
        run: |
          grep -E "^## v${{ steps.resolve.outputs.version }}( |$)" CHANGELOG.md \
            || { echo "::error::add a '## v${{ steps.resolve.outputs.version }}' section to CHANGELOG.md"; exit 1; }
      - name: Check CI green at this SHA
        env:
          GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}
        run: |
          set -euo pipefail
          runs=$(gh api "repos/${{ github.repository }}/actions/workflows/ci.yml/runs?head_sha=${{ steps.resolve.outputs.sha }}" --jq '.workflow_runs[] | select(.conclusion=="success") | .id' | head -1)
          if [[ -z "$runs" ]]; then
            echo "::error::no successful CI run for SHA ${{ steps.resolve.outputs.sha }}"
            exit 1
          fi
      - name: MANIFEST drift check
        run: |
          cd zip && grep -E '^[0-9a-f]{64}  ' bin/MANIFEST | sha256sum -c --status

  prep-image:
    needs: verify
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: docker/setup-buildx-action@v3
      - name: Build image
        uses: docker/build-push-action@v5
        with:
          context: .
          file: docker/Dockerfile
          tags: gbl-chainload-build:latest
          cache-from: type=gha
          cache-to: type=gha,mode=max
          outputs: type=docker,dest=/tmp/image.tar
      - uses: actions/upload-artifact@v4
        with:
          name: build-image
          path: /tmp/image.tar

  build-host-tools:
    needs: [verify, prep-image]
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
        with: { submodules: false }
      - uses: actions/download-artifact@v4
        with: { name: build-image, path: /tmp }
      - run: docker load -i /tmp/image.tar
      - run: bash scripts/build-cross-tools.sh all
      - name: Assemble per-platform tool zips
        run: |
          set -euo pipefail
          VER=${{ needs.verify.outputs.version }}
          for plat in linux windows macos; do
            stage="release-stage/gbl-chainload-tools-${plat}-v${VER}"
            mkdir -p "$stage/bin"
            cp dist/${plat}/* "$stage/bin/"
            cp tools/RELEASE_README.md "$stage/README.md"
            cp VERSION "$stage/VERSION"
            cp scripts/efisp-package.py "$stage/"
            ( cd "$stage/.." && zip -r "gbl-chainload-tools-${plat}-v${VER}.zip" "gbl-chainload-tools-${plat}-v${VER}" )
          done
      - uses: actions/upload-artifact@v4
        with:
          name: host-tool-zips
          path: release-stage/*.zip

  build-zips:
    needs: verify
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
        with: { submodules: recursive }
      - name: Build all five installer ZIPs
        run: |
          set -euo pipefail
          VER=${{ needs.verify.outputs.version }}
          mkdir -p release-stage
          for m in diag graft mode-0-install mode-1-install mode-2-install; do
            bash scripts/build-recovery-zip.sh --mode "$m"
            mv "dist/gbl-chainload-${m}.zip" "release-stage/gbl-chainload-${m}-v${VER}.zip"
          done
      - uses: actions/upload-artifact@v4
        with:
          name: installer-zips
          path: release-stage/*.zip

  release:
    needs: [verify, build-host-tools, build-zips]
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/download-artifact@v4
        with: { name: host-tool-zips, path: assets }
      - uses: actions/download-artifact@v4
        with: { name: installer-zips, path: assets }
      - name: Compute SHA256SUMS
        run: ( cd assets && sha256sum *.zip > SHA256SUMS )
      - name: Build release notes
        env:
          GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}
        run: |
          set -euo pipefail
          VER=${{ needs.verify.outputs.version }}
          awk -v ver="v${VER}" '
            $0 ~ "^## " ver "( |$)"      { p=1; next }
            p && $0 ~ "^## v"             { p=0 }
            p                              { print }
          ' CHANGELOG.md > prologue.md
          {
            cat prologue.md
            printf '\n\n---\n\n'
            gh api -X POST "repos/${{ github.repository }}/releases/generate-notes" \
              -f tag_name="v${VER}" \
              -f target_commitish="${{ needs.verify.outputs.sha }}" \
              --jq .body
          } > release-notes.md
      - name: Create draft release
        env:
          GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}
        run: |
          set -euo pipefail
          VER=${{ needs.verify.outputs.version }}
          if [[ "${{ github.event_name }}" == "push" ]]; then
            tag_or_target="v${VER}"
          else
            tag_or_target="v${VER} --target ${{ needs.verify.outputs.sha }}"
          fi
          # shellcheck disable=SC2086
          gh release create $tag_or_target \
            --draft \
            --title "gbl-chainload v${VER}" \
            --notes-file release-notes.md \
            assets/*.zip assets/SHA256SUMS
```

- [ ] **Step 2: Verify YAML + job graph**

```bash
python3 -c 'import yaml; wf = yaml.safe_load(open(".github/workflows/release.yml")); jobs = wf["jobs"]; assert set(jobs) == {"verify", "prep-image", "build-host-tools", "build-zips", "release"}, jobs'
command -v actionlint >/dev/null 2>&1 && actionlint .github/workflows/release.yml || echo "(actionlint not installed)"
```

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/release.yml
git commit -m "feat(ci): release workflow

Tag (v*) and workflow_dispatch triggered. Verify → prep-image →
{build-host-tools, build-zips} → release(draft). Reuses cached
docker image, runs no tests (gates on green ci.yml at the SHA),
posts draft GH Release with SHA256SUMS + CHANGELOG prologue +
auto PR list."
```

---

## Task 9: End-to-end dispatch smoke test

**Goal:** Trigger the release workflow via `workflow_dispatch` with version `2.2.0`, watch it succeed end-to-end, verify the resulting draft Release has all nine expected assets, and verify `SHA256SUMS` round-trips locally.

**Files:** none (operational task — no source changes).

**Acceptance Criteria:**
- [ ] `gh workflow run release.yml -f version=2.2.0` triggers a run that completes with `conclusion=success` on every job.
- [ ] A draft release titled `gbl-chainload v2.2.0` exists on the Releases page.
- [ ] The draft has exactly nine assets: 5 installer zips + 3 tool zips + `SHA256SUMS`.
- [ ] Downloading all nine assets and running `sha256sum -c SHA256SUMS` reports `OK` for all eight zips.
- [ ] The notes body contains the CHANGELOG v2.2.0 prologue **and** an auto-generated `## What's Changed` PR list.

**Verify:**

```bash
gh workflow run release.yml -f version=2.2.0
sleep 5
run_id=$(gh run list --workflow=release.yml --limit 1 --json databaseId --jq '.[0].databaseId')
gh run watch "$run_id" --exit-status
draft_id=$(gh release list --json tagName,isDraft --jq '.[] | select(.isDraft and .tagName=="v2.2.0") | .tagName' | head -1)
gh release view "$draft_id" --json assets --jq '.assets | length'
# Expected: 9
mkdir -p /tmp/release-verify
( cd /tmp/release-verify && gh release download "$draft_id" --pattern '*' && sha256sum -c SHA256SUMS )
# Expected: 8 OK lines
gh release view "$draft_id" --json body --jq .body | grep -F "Highlights"  # CHANGELOG prologue marker
gh release view "$draft_id" --json body --jq .body | grep -F "What's Changed"  # auto PR list marker
```

**Steps:**

- [ ] **Step 1: Push everything to main (each task's commits already pushed per CLAUDE.md PR rules — confirm)**

```bash
git status
git log origin/main..HEAD --oneline
# Expected: empty (all task commits already landed)
```

- [ ] **Step 2: Dispatch the workflow**

```bash
gh workflow run release.yml -f version=2.2.0
```

- [ ] **Step 3: Watch the run**

```bash
sleep 5
run_id=$(gh run list --workflow=release.yml --limit 1 --json databaseId --jq '.[0].databaseId')
gh run watch "$run_id" --exit-status
```

If any job fails, do NOT close this task — fix the failure (likely in Task 8's YAML), re-push, re-dispatch. Iterate until green.

- [ ] **Step 4: Inspect the draft release**

```bash
gh release view v2.2.0 --json isDraft,assets,body
```

Expected:
- `isDraft: true`
- `assets: [ ... 9 items ... ]`
- `body` contains both the CHANGELOG prologue and the auto-generated PR list.

- [ ] **Step 5: Round-trip SHA256SUMS**

```bash
mkdir -p /tmp/release-verify && cd /tmp/release-verify
gh release download v2.2.0
sha256sum -c SHA256SUMS
```

Expected: 8 `OK` lines.

- [ ] **Step 6: Delete the smoke-test draft (optional, only if it's not the actual release)**

If this dispatch was a dry-run before the actual `v2.2.0` tag:

```bash
gh release delete v2.2.0 --yes
```

If this dispatch IS the intended release-cut, leave it as a draft for human review/publish.

- [ ] **Step 7: Note completion**

This task has no commit. Closing it documents that the workflow works end-to-end.

---

## Self-Review

**Spec coverage:**

| Spec section | Covered by |
|---|---|
| §2 Triggers (tag + dispatch) | Task 8 |
| §3.1 Verify (version, CHANGELOG, CI green, MANIFEST) | Task 8 |
| §3.2 prep-image (docker cache) | Task 8 |
| §3.3 build-host-tools | Task 8 (+ Task 4 for the linux arm + Task 5 for efisp-package.py) |
| §3.4 build-zips | Task 8 (+ Task 6 for the placeholder substitution) |
| §3.5 release (notes assembly) | Task 8 |
| §4.1 VERSION file | Task 1 |
| §4.2 Consumers (dsc, ABL, tools, zip, efisp-package) | Tasks 1, 3, 5, 6 |
| §4.3 Visible surfaces (menu row, getvar, on-screen, ui_print, --version) | Tasks 1 (on-screen via Entry.c), 2 (menu + getvar), 3 (--version), 6 (ui_print) |
| §4.4 Remnant cleanup | Tasks 1 (Entry.c + dsc), 3 (gbl-pack 1.0.0) |
| §5 CHANGELOG mechanic | Tasks 7 (file), 8 (extractor) |
| §6 File inventory | All tasks |
| §8 Release ritual | Implicit; verifier gates enforce it |

**Placeholder scan:** Done. No `TBD`/`TODO`/"fill in"/"similar to" tokens in task bodies. All step code blocks have actual content.

**Type consistency:** `GBL_CHAINLOAD_VERSION` macro used consistently in Tasks 1, 2. `GBL_TOOL_VERSION` macro used consistently in Task 3. `__GBL_VERSION__` placeholder used consistently in Tasks 6, 7. `VERSION` filename used consistently throughout.

**Notes / risks:**
- Task 6 touches the zip submodule; per project rules direct main pushes need user authorization. The implementer should ask before pushing.
- Task 8's `gh release create --target` syntax mixed into a positional arg via `$tag_or_target` uses `shellcheck disable=SC2086` intentionally — keep that pragma.
