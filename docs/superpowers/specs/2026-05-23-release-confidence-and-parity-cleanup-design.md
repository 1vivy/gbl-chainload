# Release confidence + parity cleanup — design

**Date:** 2026-05-23
**Status:** approved, ready to plan

## Goal

Consolidate three pieces of housekeeping into one PR so the v2.3.4 release
(and every future release) is a clean single-purpose version bump:

1. **Retire parity-contract goldens** now that PR #44 has shipped the Rust
   tooling and on-device validation is green.
2. **Make the release workflow trustworthy** — CI invariants catch every
   drift between parent + submodules so "merge release PR, push tag, get
   draft release" is the workflow without surprises.
3. **CI optimizations** that are cheap and well-bounded (no EDK2 cache).

After this lands, the v2.3.4 PR is a 2-file diff (`VERSION` + `CHANGELOG.md`),
explicit on main per CLAUDE.md.

## Background

Three things converged to motivate this:

- **PR #46's CI fails on golden 060.** `gbl pack` embeds the project
  VERSION via `include_str!("../../VERSION")` at compile time. Goldens
  captured pre-migration with VERSION=2.2.2 (when the deleted C `gbl-pack`
  was authoritative) don't byte-match a VERSION=2.3.4 rebuild. The Rust
  port has fulfilled its byte-parity contract; the goldens are now scaffolding
  that breaks on legitimate version bumps.
- **Submodule sync has bitten us already.** This session re-pushed an
  edk2 commit twice, fixed a zip MANIFEST drift that snuck into the
  PR #45 squash, and discovered a stale `cd85753b97` claim. The release
  workflow only checks `zip/bin/MANIFEST` internal consistency — nothing
  verifies that the parent's submodule pointer actually matches the
  vendored-artifact provenance recorded inside the submodule, or that the
  pinned commit is reachable from the submodule's `origin/main`.
- **Release authoring is too manual.** Today, cutting a release means:
  bump VERSION, write CHANGELOG, run `zip/update-tools.sh` (which itself
  rebuilds the EFI + refreshes zip's MANIFEST + commits in the
  submodule), bump parent's zip pointer, push branch + open PR, push tag
  after merge. Easy to miss a step.

## Architecture

Three independent units that land together:

```
┌─────────────────────────────────────────────────────────────────┐
│                                                                 │
│  Unit A — Golden retirement                                     │
│    Delete tests/host/goldens/ entirely.                         │
│    Strip cmp -s / diff goldens assertions from 18 test files.   │
│    Replace 5 textual goldens with inline grep -qE assertions.   │
│                                                                 │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  Unit B — Release confidence system                             │
│    Layer B1 (CI invariants) — verify job extensions:            │
│      · EFI parity:      sha256(dist/gbl-chainload.efi)          │
│                           == sha256(zip/base/gbl-chainload.efi) │
│      · zip provenance:  zip/bin/MANIFEST `# parent-commit:`     │
│                           matches parent SHA being tagged       │
│      · reachability:    submodule pointer reachable from each   │
│                           submodule's origin/main               │
│    Layer B2 (Author script) — scripts/release.sh X.Y.Z:         │
│      · bump VERSION + scaffold CHANGELOG section                │
│      · run zip/update-tools.sh (rebuilds EFI, refreshes         │
│          zip/bin/MANIFEST, commits in zip)                      │
│      · bump parent's zip pointer, commit on release branch      │
│      · push branch, open PR                                     │
│    New release.yml `build-efi` job uploads firmware payload as  │
│    a release asset; release job folds it into SHA256SUMS.       │
│                                                                 │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  Unit C — CI optimizations                                      │
│    · cargo registry + git cache, keyed on Cargo.lock            │
│    · concurrency groups (cancel-in-progress on non-default      │
│        refs) for ci.yml, host-tools.yml, release.yml            │
│    · NOT: EDK2 build cache (banner/version passthrough)         │
│    · NOT: target/ cache (size / invalidation cost > benefit)    │
│                                                                 │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  Unit D — CLAUDE.md flexibility tweak                           │
│    · Add: version bumps land as their own focused PR            │
│    · Soften: single-commit PRs are fine; ceremony not required  │
│        — keep branch+PR but don't force multi-commit branches   │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## Unit A — Golden retirement

### What we delete

Every directory under `tests/host/goldens/`. The `MANIFEST` file at
`tests/host/goldens/MANIFEST` (whose comment header explicitly says
"frozen pre-migration C tool outputs… parity contract"). All 18 test
files that reference `tests/host/goldens/` get their assertions stripped.

### Replacement strategy

| Golden type        | Replacement                                                     |
|--------------------|-----------------------------------------------------------------|
| Binary `.bin`/`.efi` (parity contract) | Delete the `cmp -s`/`diff` line. The test's existing parser/roundtrip/status checks stay. No new assertions needed — the Rust impl is authoritative. |
| Textual `.txt`/`.toml` (behavior contract) | Replace `diff actual goldens/…` with 2-4 inline `grep -qE "<field>"` lines that capture the specific behavior the golden was encoding. |

### Textual golden replacements (per file)

- `074/list.txt` (vbmeta-graft list, 2 lines) → check `partition=recovery type=hash graftable=yes` present, no error tokens.
- `087/baseline.toml` (mode2-profile derive output) → check `version = 1`, `is_unlocked = 0`, `system_version = 0x40000`, and `spl` present. Skip the fixture-path comment (was always brittle).
- `089/{manifest,manifest2,ok}.txt` (gblp1-inspect) → check `result: ok`, header `magic=ok version=1`, expected entry count.
- `090/lh.txt` + `lh-corrupt.txt` (list-hash forensic output) → check `verdict=mismatch`/`verdict=ok` and partition name.
- `091/default-list.txt` (vbmeta-graft.py default) → same shape as 074 — check `partition=recovery type=hash graftable=yes`.

## Unit B — Release confidence system

### B1: CI invariants

Three new steps in release.yml's `verify` job (runs before any build):

```yaml
- name: EFI parity (built fresh == vendored in zip)
  run: |
    # Runs after build-efi via job dependency; downloads efi-payload
    # artifact. Asserts sha256 equality with zip/base/gbl-chainload.efi.
    sha256sum dist/gbl-chainload.efi zip/base/gbl-chainload.efi | \
      awk 'NR==1 {a=$1} NR==2 {b=$1} END { if (a != b) { print "EFI drift:", a, "vs", b; exit 1 }}'

- name: Submodule provenance — zip MANIFEST parent-commit
  run: |
    parent=$(awk '/^# parent-commit:/ {print $3}' zip/bin/MANIFEST)
    tag_sha="${{ steps.resolve.outputs.sha }}"
    if [ "$parent" != "$tag_sha" ]; then
      echo "::error::zip/bin/MANIFEST parent-commit '$parent' != tag SHA '$tag_sha'"
      echo "  run zip/update-tools.sh to refresh vendored artifacts"
      exit 1
    fi

- name: Submodule pointer reachability
  run: |
    for sub in zip edk2; do
      pin=$(git ls-tree HEAD "$sub" | awk '{print $3}')
      if ! git -C "$sub" merge-base --is-ancestor "$pin" origin/main 2>/dev/null; then
        echo "::error::$sub submodule pinned to $pin which is not reachable from $sub/origin/main"
        exit 1
      fi
    done
```

The EFI parity check actually lives in the new `build-efi` job (it has the
fresh build artifact); the verify job consumes its output. Order:
verify → prep-image → build-efi → (parity assertion in build-efi) →
build-host-tools, build-zips → release.

### B2: Author script

`scripts/release.sh X.Y.Z` (POSIX shell):

```text
1. Sanity:  on main, working tree clean, X.Y.Z is valid semver, not already tagged.
2. Branch:  create release-prep-vX.Y.Z off main.
3. Bump:    write X.Y.Z to VERSION.
4. CHANGELOG: insert `## vX.Y.Z — <today>` section template at top; open $EDITOR
            for the user to fill in highlights (if no $EDITOR, just print the
            location and continue — user can edit before commit).
5. Zip refresh: run zip/update-tools.sh (which builds EFI in-container,
                refreshes zip/bin/MANIFEST with new parent SHA preview,
                commits in zip submodule). If the script doesn't exist or
                fails, error out cleanly with the manual recovery path.
6. Bump zip pointer: git add zip, commit.
7. Commit VERSION + CHANGELOG.
8. Push branch.
9. Open PR (gh pr create) with title "release: X.Y.Z" and body referencing
   the auto-detected commits since last tag.
10. Print next steps: review PR, merge, then `git tag vX.Y.Z && git push origin vX.Y.Z`.
```

### B3: build-efi job

(Already drafted on this branch — keep, plus add the parity step.) Builds
inside the prep-image via `bash scripts/build.sh --no-recovery --no-host`,
renames to `gbl-chainload-vX.Y.Z.efi`, asserts byte-equality with
`zip/base/gbl-chainload.efi`, uploads as artifact for the `release` job.

## Unit C — CI optimizations

### Cargo cache

Across `ci.yml`, `host-tools.yml`, and `release.yml`'s cargo-touching
jobs, add:

```yaml
- uses: actions/cache@v4
  with:
    path: |
      ~/.cargo/registry/index
      ~/.cargo/registry/cache
      ~/.cargo/git/db
    key: ${{ runner.os }}-cargo-${{ hashFiles('**/Cargo.lock') }}
    restore-keys: |
      ${{ runner.os }}-cargo-
```

Deliberately **not** caching `target/`: workspace member sources change
often, target invalidation is finicky, and the dep graph is the slow
part. Registry+git db gives ~80% of the wall-clock benefit at ~5% of
the cache size.

### Concurrency groups

Each workflow gets:

```yaml
concurrency:
  group: ${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: ${{ github.ref != 'refs/heads/main' }}
```

Cancels in-flight runs on force-pushes to feature branches. Never cancels
main (we want every commit to main's history to record CI status).

### Not in scope

EDK2 build cache (banner + version passthrough require fresh link;
caching would silently ship stale strings). Target/ cache (see above).
Self-hosted runners (overkill).

## Unit D — CLAUDE.md flexibility tweak

Edit under "Workflow: branch then PR":

```diff
- Work happens on feature branches; landing on `main` is via PR.
+ Work happens on feature branches; landing on `main` is via PR.

- - Never commit to or push `main` directly.
- - Feature branches are otherwise unrestricted: commit early, commit often,
-   iterate freely. The PR grows new commits as feedback comes in.
- - Hot-fix-style "tiny" / "obvious" changes are not an exception to the
-   branch-and-PR rule.
+ - Never commit to or push `main` directly.
+ - Branch+PR applies to all changes, but the ceremony scales with the
+   change. Single-commit PRs are fine for small fixes; multi-commit
+   feature branches are fine for larger work — iterate freely on the
+   branch, the PR grows new commits as feedback comes in.
+ - **Version bumps (`VERSION` + `CHANGELOG.md`) land as their own focused
+   PR — explicit on main, no bundling with feature work.** Use
+   `scripts/release.sh X.Y.Z` to scaffold it.
```

## Components — file map

```
Unit A  tests/host/goldens/                         delete (entire tree)
        tests/host/{060,061,064,067,069,072,074,    edit (strip golden lines,
                    081,082,083,085,087,088,089,    add inline grep checks
                    090,091,094,096}_*.sh           where textual goldens
                                                    were used)

Unit B  .github/workflows/release.yml               edit (single-binary loop,
                                                    new build-efi job with
                                                    parity check, verify-job
                                                    provenance + reachability)
        scripts/release.sh                          create
        tools/RELEASE_README.md                     already-edited (keep)

Unit C  .github/workflows/ci.yml                    edit (concurrency,
                                                    cargo cache)
        .github/workflows/host-tools.yml            edit (concurrency,
                                                    cargo cache)
        .github/workflows/release.yml               edit (concurrency,
                                                    cargo cache in jobs
                                                    that touch cargo)

Unit D  CLAUDE.md                                   edit
```

## Data flow

### Release flow (new)

```
author     scripts/release.sh 2.3.5
            ↓
         release-prep-v2.3.5 branch w/ VERSION + CHANGELOG + zip pointer bumps
            ↓
         PR opens → CI runs (existing checks pass)
            ↓
         author reviews + merges PR → main now has VERSION 2.3.5
            ↓
author     git tag v2.3.5 && git push origin v2.3.5
            ↓
         release.yml triggered
            ↓
         verify (semver, CHANGELOG section, MANIFEST drift)
            ↓
         prep-image (cached)
            ↓
         build-efi (fresh build) → EFI parity check vs zip/base/gbl-chainload.efi
            ↓                       ┌─ if differ: FAIL with "run zip/update-tools.sh"
            ↓                       └─ if equal:  pass
         submodule-provenance check (MANIFEST parent-commit == tag SHA)
            ↓
         submodule-reachability check (pinned SHAs reachable from submodule mains)
            ↓
         build-host-tools, build-zips (parallel)
            ↓
         release job: assemble assets, SHA256SUMS, draft release
```

### Test flow (after Unit A)

Each formerly-golden test still does its real work (parse/roundtrip/size
checks) but skips the byte-identity assertion. Textual-golden tests carry
inline schema checks in their place. tests/host/MANIFEST file removed;
tests/host/README.md updated if it references goldens.

## Error handling

- **EFI drift** → release.yml fails with explicit message + recovery hint
  (`run zip/update-tools.sh`).
- **MANIFEST parent-commit mismatch** → fails with same hint.
- **Submodule unreachability** → fails with submodule name + pinned SHA.
- **scripts/release.sh failure** → fails fast, never leaves a half-applied
  state (uses a trap on EXIT to roll back partial branch creation if the
  zip refresh fails midway).
- **Cargo cache miss** → no-op, falls through to fresh download — never a
  hard failure.

## Testing

- Unit A: every formerly-golden test runs and passes locally + in CI
  (the parser/roundtrip checks alone are sufficient).
- Unit B: `bash scripts/release.sh 2.3.4-dryrun` on a throwaway branch
  confirms the script's branch creation + commit shape. Release-workflow
  changes verified by tagging v2.3.4 (the follow-up PR) and observing
  the draft release land.
- Unit C: cargo cache hit-rate measured on PR #46's CI run (before) vs.
  this PR's CI run (after). Concurrency groups observed by force-pushing
  the branch and watching the old run cancel.
- Unit D: documentation-only; no test.

## Tradeoffs

- **Golden deletion is one-way.** If a future refactor introduces a
  subtle byte-level regression in `gbl pack` output, no golden catches
  it. The roundtrip + parser checks catch every realistic regression,
  but a "this byte should be X" surgical bug could slip through. Net
  judgment: parity scaffolding is more harm than help once authority has
  moved to the Rust impl.
- **scripts/release.sh hides the zip submodule dance behind one command,**
  which is great for cutting releases but obscures what's happening for
  someone unfamiliar with the layout. Mitigated by the script being
  small (~50 lines), well-commented, and using familiar git + gh
  commands the author would otherwise run by hand.
- **Cargo registry+git cache size grows over time** (no eviction in
  GitHub Actions cache). When it crosses ~5 GB it'll cause cache push
  failures. Mitigation: include the GHA cache TTL semantics in the
  cache key (or rotate the key version manually if it ever bites).
- **PR #46 force-push rewrites history.** Acceptable since PR #46
  hasn't been reviewed; reviewer sees only the new content.

## Out of scope

- v2.3.4 release itself — separate PR, single-purpose.
- Workflow-level deduplication of MANIFEST drift check (now in 3 places).
  Leave as-is until it becomes a maintenance burden.
- Multi-platform native runners (release.yml already uses
  windows-latest/macos-latest for tests; the cross-build itself runs
  in docker on ubuntu-latest, which is appropriate).
- Self-hosted runners.

## Open questions

None — design approved verbally in the brainstorm session.
