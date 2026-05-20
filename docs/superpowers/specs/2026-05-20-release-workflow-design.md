# Release workflow + project versioning — design

**Date:** 2026-05-20
**Status:** design / pending review
**Scope:** GitHub Actions release workflow, host-tool packaging, single
source-of-truth project version with user-visible surfaces

## 1. Goal

Ship a tagged release of gbl-chainload that bundles:

- All five installer ZIPs (diag, graft, mode-0/1/2-install).
- Host-side tools for Linux x86_64, Windows x64, and macOS universal,
  each packaged with the `efisp-package.py` helper.
- A single `SHA256SUMS` covering every release asset.
- Hand-written prologue plus GitHub's auto-generated PR list as release
  notes.

A second outcome: surface the project version everywhere a user can see
it (fastboot menu, fastboot getvar, recovery installer ui_print, host
tool `--version`, on-screen banner from the chainloaded EFI), driven
from a single `VERSION` file. The previously orphaned
`GBL_CHAINLOAD_VERSION` log line becomes only one of many consumers of
that file.

## 2. Triggers

Both supported, single workflow file.

- **`push` of `v*` tag** — production path. Version is derived from the
  tag with the leading `v` stripped (`v2.2.0` → `2.2.0`).
- **`workflow_dispatch`** — primary release-cut path. Accepts a
  `version` input string. `gh release create` will create the
  `v<version>` tag if it doesn't already exist (it's required to
  attach the draft); `--target <sha>` pins the tag to the workflow's
  commit SHA. (Earlier spec language claimed dispatch wouldn't create
  a tag — that was overspecified. A release without a tag isn't
  coherent, and the operational cost of a `dispatch-<ver>` cleanup
  namespace was higher than the benefit.)

The dispatch input must match the form `X.Y.Z` (regex enforced in the
first job); anything else fails fast.

## 3. Job graph

```
verify  ──► prep-image  ──► build-host-tools  ──┐
        └─► build-zips  ────────────────────────┴──► release (draft)
```

Tests are **not** rerun in the release workflow.

### 3.1 `verify`

Runs on `ubuntu-latest`. Order matters — cheapest fails first.

1. **Version equality.** Read `VERSION` at the release SHA. Compare to
   the tag-derived value (or dispatch input). Mismatch ⇒ fail with
   "tag `vX.Y.Z` does not match VERSION `A.B.C`."

2. **CHANGELOG presence.** Grep `CHANGELOG.md` for `^## v<ver>`. Missing
   ⇒ fail with "add a `## v<ver>` section to CHANGELOG.md before tagging."

3. **CI green at this SHA.** Call `gh api repos/{owner}/{repo}/actions/runs?head_sha=<sha>` and assert at
   least one `CI` workflow run on this commit completed with
   `conclusion=success`. For `workflow_dispatch` from a non-main SHA
   that has never run `CI`, fall back to invoking `ci.yml` as a
   reusable workflow on the current ref.

4. **MANIFEST drift check.** `cd zip && grep -E '^[0-9a-f]{64}  ' bin/MANIFEST | sha256sum -c --status`. Same line `build-recovery-zip.sh` uses.

5. Outputs `version` for downstream jobs.

### 3.2 `prep-image`

Builds `gbl-chainload-build:latest` via `docker buildx build` with
`--cache-to type=gha,mode=max --cache-from type=gha`, keyed by hash of
`docker/` + `Dockerfile`. Saves the image as a tar artifact for
downstream `docker load`.

Typical cache-hit time ≤30 s.

### 3.3 `build-host-tools`

Single job (not a matrix). `docker load` the cached image, then run
`scripts/build-cross-tools.sh all` — which after this design's
modifications produces `dist/linux/`, `dist/windows/`, `dist/macos/`
all from one docker container invocation.

For each platform, the job assembles the release zip:

```
gbl-chainload-tools-<plat>-v<ver>/
├── README.md          # from tools/RELEASE_README.md (verbatim)
├── SHA256SUMS         # per-file inside the zip
├── VERSION            # copy of root VERSION (consumed by efisp-package.py)
├── efisp-package.py
└── bin/
    ├── fv-unwrap[.exe]
    ├── abl-patcher[.exe]
    ├── gbl-pack[.exe]
    ├── gbl-commit[.exe]
    ├── vbmeta-graft[.exe]
    ├── mode2-profile[.exe]
    └── gblp1-inspect[.exe]
```

Linux binaries are zig-built static x86_64 (same cross-toolchain
`build-cross-tools.sh` already uses for Windows/macOS — extending its
target list, not introducing a second toolchain).

### 3.4 `build-zips`

`ubuntu-latest`, no docker. Five sequential calls:

```
for m in diag graft mode-0-install mode-1-install mode-2-install; do
  bash scripts/build-recovery-zip.sh --mode "$m"
  mv dist/gbl-chainload-${m}.zip dist/gbl-chainload-${m}-v${VER}.zip
done
```

Each ZIP packages only already-vendored bytes from `zip/`; no compile.
Total wall-clock typically <30 s.

`scripts/build-recovery-zip.sh` is modified to substitute the
`GBL_VERSION` placeholder in the staged
`META-INF/com/google/android/update-binary` before zipping. The
installer script's existing `ui_print` header line now reads
`gbl-chainload v<VERSION>` on the recovery screen.

### 3.5 `release`

Collects:

- The five installer ZIPs from `build-zips`.
- The three host-tool ZIPs from `build-host-tools`.

Computes a top-level `SHA256SUMS` over all eight files. Posts a **draft**
GitHub Release using `gh release create`:

- For tag trigger: attached to the pushed tag.
- For dispatch trigger: `--target <sha>` pins the tag to the workflow
  SHA; the tag itself is created by `gh release create` if absent.
- `--draft` always.
- `--notes-file release-notes.md`, built in-job by:
  1. Extract `## v<ver>` section from `CHANGELOG.md` (awk between
     `^## v<ver>` and the next `^## v`).
  2. Append a `---` separator.
  3. Append the auto PR list fetched via
     `gh api repos/{owner}/{repo}/releases/generate-notes -F tag_name=v<ver> -F target_commitish=<sha>` (pulls the
     same content GitHub's "auto-generate notes" button produces, honoring
     `.github/release.yml` grouping).

Single `gh release create` call — no `--generate-notes` flag, no
follow-up `gh release edit`.

A `.github/release.yml` config groups the auto PR list by label:

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

## 4. Versioning — single source of truth

### 4.1 The `VERSION` file

Top-level `VERSION` containing one line of the form `X.Y.Z`. No leading
`v`. No suffixes. Both triggers enforce the same `X.Y.Z` regex on the
released version.

Initial content: the next-release value the user chooses (suggested
`2.2.0`, supersedes the current `2.0` baked in `.dsc`).

### 4.2 Consumers — build time

All consumers read `VERSION` at build time. No runtime file reads.

| Consumer | Mechanism |
|---|---|
| `GblChainloadPkg.dsc` | `scripts/build.sh` exports `GBL_CHAINLOAD_VERSION=$(cat VERSION)` before `build`. `.dsc` line 60 `DEFINE GBL_CHAINLOAD_VERSION = 2.0` is **removed**. `.dsc` line 154 (CC_FLAGS reference) stays — it now sources the env var. |
| Patched ABL build (`QcomModulePkg`) | Same `GBL_CHAINLOAD_VERSION` env var; `scripts/build.sh` arranges for it to reach the QcomModulePkg build too. |
| `tools/<each>/Makefile` | Adds `GBL_TOOL_VERSION := $(shell cat $(SELF)/../../VERSION)` and `-DGBL_TOOL_VERSION='"$(GBL_TOOL_VERSION)"'` to CFLAGS. |
| `scripts/build-recovery-zip.sh` | Reads `$(cat VERSION)`, substitutes a `__GBL_VERSION__` placeholder in the staged `update-binary`. |
| `scripts/efisp-package.py` | Reads `VERSION` adjacent to the script (script-dir relative). `--version` flag prints it. |

### 4.3 User-visible surfaces

| Surface | File | What user sees |
|---|---|---|
| Patched-ABL fastboot menu | `edk2/QcomModulePkg/Library/BootLib/FastbootMenu.c` — new row after the `MODE -` row | `VERSION - X.Y.Z` on the device screen. |
| Patched-ABL fastboot getvar | `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c` — new `FastbootPublishVar("gbl-chainload_version", GBL_CHAINLOAD_VERSION)` alongside `_mode`/`_date`/`_auto` | `fastboot getvar gbl-chainload_version` → `X.Y.Z`. |
| Chainloaded EFI on-screen | `GblChainloadPkg/Application/GblChainload/Entry.c` `CommonEarlyInit` | Existing `SCR_PRINT` lines get a leading `gbl-chainload v<ver>: …` prefix. Visible during `oem boot-efi`. |
| Chainloaded EFI log | `Entry.c:122` (existing `GBL_INFO`) | Already prints `GBL_CHAINLOAD_VERSION`. Now sourced from the VERSION file. Bundled into diag's `/sdcard/` log dump. |
| Installer ZIP (recovery screen) | `zip/META-INF/com/google/android/update-binary` | `ui_print "gbl-chainload v<ver>"` header. |
| Each host tool's `--version` | `tools/<each>/<tool>.c` | Tool prints `X.Y.Z` and exits zero. Also appended to usage banner ("Usage: <tool> ... (vX.Y.Z)"). |
| `efisp-package.py --version` | Already noted. | `X.Y.Z`. |
| Release asset filenames | Build wiring | Documented. |

### 4.4 Remnant cleanup

| Site | Today | Action |
|---|---|---|
| `Entry.c:39-40` | Fallback `#define GBL_CHAINLOAD_VERSION "v2"` | Replace with `#error "GBL_CHAINLOAD_VERSION must be defined by build"`. Build macro becomes mandatory. |
| `GblChainloadPkg.dsc:60` | `DEFINE GBL_CHAINLOAD_VERSION = 2.0` | Remove. |
| `tools/gbl-pack/gbl-pack.c:64` | `in.packer_version = "gbl-pack 1.0.0";` | Replace with `in.packer_version = "gbl-pack " GBL_TOOL_VERSION;` (built-in concat of two string literals). |

**Confirmed-not-remnants** (protocol / schema / Android-domain — leave):

- `tools/shared/gblp1.h: GBLP1_VERSION` — GBLP1 container protocol version.
- `tools/shared/gbl_mode2_profile.h: GBL_M2P_VERSION`, `M2P_VERSION = 1` in `tools/mode2-profile/*` — m2p schema version.
- `tools/shared/gbl_staged_buffer.h: GBL_STAGED_BUFFER_VERSION` — staged-buffer ABI.
- Android `os_version` / `system_version` / `system_spl` in `tools/mode2-profile/*` — vbmeta-domain.
- `tools/gblp1-inspect/gblp1-inspect.c` parsing of header `version` field — protocol parsing.

Old git tags (`v2.0.0`, `v2.0.0-plan1-foundation`, `v2.0.0-plan2-mode0-logfs-patch9v2`, `v2.1.0`) are left as historical waypoints.

## 5. Release notes

`CHANGELOG.md` is the curated source. Format:

```markdown
# Changelog

## v2.2.0 — 2026-MM-DD

(prologue: highlights, upgrade notes, breaking changes,
flashing instructions if any)

## v2.1.0 — 2026-05-XX

…
```

Initial commit includes seeded sections for known prior tags (`v2.0.0`,
`v2.1.0`) reconstructed from existing release-prep branch + commit
history (best-effort; users may revise).

The workflow extracts the `## v<ver>` section as the prologue. Combined
release body:

```
<CHANGELOG prologue>

---

<GitHub auto-generated PR list>
```

If the CHANGELOG section is missing, `verify` fails before any build
runs. There is no "empty draft, fill in later" path.

## 6. File inventory

### 6.1 New files

| Path | Purpose |
|---|---|
| `.github/workflows/release.yml` | The workflow. |
| `.github/release.yml` | PR-label categories for auto-generated notes. |
| `CHANGELOG.md` | Curated release notes per version, seeded with prior tags. |
| `tools/RELEASE_README.md` | Source for the per-platform tool-zip README. |
| `VERSION` | One line: `X.Y.Z`. Source of truth. |

### 6.2 Modified files

| Path | Change |
|---|---|
| `scripts/build.sh` | Export `GBL_CHAINLOAD_VERSION=$(cat VERSION)` for both `GblChainloadPkg` and `QcomModulePkg` builds. |
| `scripts/build-cross-tools.sh` | Add `linux` arm; `all` now means win+macOS+linux. Linux is musl-static x86_64 inside the docker image. |
| `scripts/build-recovery-zip.sh` | Read `VERSION`, substitute `__GBL_VERSION__` placeholder in `update-binary` before zipping. |
| `scripts/efisp-package.py` | Add `--bin-dir` flag with script-dir `bin/` fallback. Add `--version` flag reading `VERSION` from script-adjacent location. |
| `tools/<each>/Makefile` | Add `-DGBL_TOOL_VERSION='"X.Y.Z"'` (read from `../../VERSION` at make time). |
| `tools/<each>/<tool>.c` | Add `--version` flag handling + version-in-usage-banner. |
| `tools/gbl-pack/gbl-pack.c` | Replace hardcoded `"gbl-pack 1.0.0"` with `"gbl-pack " GBL_TOOL_VERSION`. |
| `GblChainloadPkg/GblChainloadPkg.dsc` | Remove `DEFINE GBL_CHAINLOAD_VERSION = 2.0`. |
| `GblChainloadPkg/Application/GblChainload/Entry.c` | Drop the `"v2"` fallback; add the `#error`. Existing `SCR_PRINT` lines get a `v<ver>:` prefix so the chainloaded EFI shows the version on-screen during `oem boot-efi`. |
| `zip/META-INF/com/google/android/update-binary` | Add a `ui_print "gbl-chainload v__GBL_VERSION__"` header line; placeholder substituted at zip-build time. |
| `edk2/QcomModulePkg/Library/BootLib/FastbootMenu.c` | New `VERSION - <ver>` row in the fastboot menu, after the `MODE -` row. |
| `edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c` | New `FastbootPublishVar("gbl-chainload_version", GBL_CHAINLOAD_VERSION)` alongside `_mode`/`_date`/`_auto`. |

### 6.3 Unchanged but exercised

- `tests/runall.sh` — invoked as a reusable workflow only on the
  `workflow_dispatch` fallback branch.
- `zip/bin/MANIFEST` drift check — same `sha256sum -c` line as
  `build-recovery-zip.sh`.
- `docker/Dockerfile` + `gbl-chainload-build:latest` — reused as-is.

## 7. Out of scope (explicit non-goals)

- Auto-bumping `VERSION` or generating it from commit count. The user
  bumps it by hand as part of the release ritual.
- Cosign / GPG / Sigstore signing. `SHA256SUMS` only.
- Homebrew, scoop, winget, PPA, AUR, Flatpak metadata.
- A separate `dispatch-<ver>` git tag namespace for dry-runs. Dispatch
  shares the production `v<ver>` tag — `gh release create` creates it
  if absent, pinned to the workflow SHA via `--target`.
- Re-signing the patched ABL or vendor partition.
- Linux arm64 host-tool build (skipped per earlier scope decision).

## 8. Release ritual

The workflow assumes this human flow at release time:

1. Bump `VERSION` to the new value.
2. Add `## v<new>` section to `CHANGELOG.md`.
3. If `.dsc` / engine source / tools / ABL patches changed since the
   last release: run `zip/update-tools.sh` to refresh
   `zip/bin/MANIFEST` and `zip/base/*.efi`. Commit the zip submodule
   bump on parent.
4. Commit + push to main; wait for `CI` to go green.
5. `git tag vX.Y.Z && git push --tags`.
6. Release workflow runs to completion; draft release appears in the
   Releases page.
7. Review notes, click Publish.

The two `verify` gates (VERSION vs tag, CHANGELOG section present)
prevent shortcuts.

## 9. Risks and mitigations

- **CHANGELOG section drift** — author forgets to add an entry, then
  has to remove the tag, push the entry, re-tag. Mitigation: gate is in
  `verify`, runs in <10 s, no artifacts built; cost of the miss is one
  tag deletion + retag.
- **`workflow_dispatch` SHA without prior CI run** — fallback invokes
  `ci.yml` as reusable; slower (~6 min added) but still single-source.
- **Linux zig-static binaries calling glibc-only APIs** — current
  tools use only POSIX + `mmap`/`stat`/SHA-256; should compile clean
  under zig's musl target. Verified at impl time by smoke-testing the
  binaries in CI's ubuntu-latest runner.
- **Docker image cache miss** — first run after a `docker/` change
  costs ~3-5 min; subsequent runs ~30 s. Acceptable.
- **ABL fastboot menu row count** — adding one row to FastbootMenu.c
  needs verification that the menu still fits the canoe screen layout.
  Manual visual check on the test phone after first build.
